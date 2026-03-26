#pragma once
#include <Arduino.h>
#include <math.h>
#include "hardware.h"
#include "utils.h"
#include "api.h"

// Firmware constant — not tunable from frontend
static constexpr float SENSOR_VELOCITY_WEIGHT = 0.3f;

struct PieceState {
  float x, y;     // estimated position in grid coords
  float vx, vy;   // velocity in grid-units/sec
  bool stuck;      // static friction not yet overcome

  void reset(float px, float py) {
    x = px; y = py;
    vx = 0; vy = 0;
    stuck = true;
  }
};

struct PhysicsParams {
  // Force model: F = voltage_scale * force_k / (d^falloff_exp + epsilon)
  float force_k          = 5.0f;
  float force_epsilon    = 0.5f;
  float falloff_exp      = 2.0f;
  float voltage_scale    = 1.0f;

  // Friction
  float friction_static  = 1.0f;
  float friction_kinetic = 3.0f;

  // Control targets
  float target_velocity  = 3.0f;   // grid-units/sec
  float target_accel     = 10.0f;  // grid-units/sec^2

  // Sensor distance model (same shape as force, inverted)
  float sensor_k         = 500.0f;
  float sensor_falloff   = 2.0f;
  float sensor_threshold = 50.0f;  // min (baseline - reading) to activate

  // Safety
  uint16_t max_duration_ms = 5000;
};

// Calibration data bridge — Board copies relevant fields here
struct CalSensorData {
  float baseline_mean;
  float piece_mean;
};

class PhysicsMove {
public:
  PhysicsMove(Hardware& hw) : hw_(hw) {}

  void setCalData(const CalSensorData* sensors, int count) {
    cal_sensors_ = sensors;
    cal_count_ = count;
  }

  MoveError execute(PieceState& piece, const uint8_t path[][2], int path_len,
                    const PhysicsParams& params) {
    if (path_len < 1) return MoveError::COIL_FAILURE;

    int coil_idx = 0;
    float cx = path[0][0], cy = path[0][1];

    // Determine movement direction
    float dx = path[path_len - 1][0] - piece.x;
    float dy = path[path_len - 1][1] - piece.y;
    bool moveX = (fabsf(dx) > fabsf(dy));

    // Activate first coil
    int8_t activeBit = coordToBit(path[0][0], path[0][1]);
    if (activeBit < 0) return MoveError::COIL_FAILURE;

    float force_mag = coilForce(0, params);
    uint8_t duty = forceToDuty(force_mag, params);
    if (!hw_.pulseBit((uint8_t)activeBit, 1, duty)) return MoveError::COIL_FAILURE;

    unsigned long t0 = millis();
    unsigned long last_tick_us = micros();

    float prev_sensor_pos = 0;
    bool prev_sensor_valid = false;
    uint8_t last_duty = duty;

    LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%d,%d)",
              path_len, piece.x, piece.y, path[path_len-1][0], path[path_len-1][1]);

    while (millis() - t0 < params.max_duration_ms) {
      // 1. Measure dt
      unsigned long now_us = micros();
      float dt = (now_us - last_tick_us) / 1000000.0f;
      last_tick_us = now_us;
      if (dt <= 0 || dt > 0.1f) dt = 0.001f;

      // 2. Compute distance to active coil and force
      float ddx = cx - piece.x;
      float ddy = cy - piece.y;
      float dist = sqrtf(ddx * ddx + ddy * ddy);
      force_mag = coilForce(dist, params);

      float fx = 0, fy = 0;
      if (dist > 0.001f) {
        fx = force_mag * (ddx / dist);
        fy = force_mag * (ddy / dist);
      }

      // 3. Friction
      if (piece.stuck) {
        if (force_mag > params.friction_static) {
          piece.stuck = false;
          LOG_BOARD("physics: static friction overcome (F=%.2f > %.2f)", force_mag, params.friction_static);
        } else {
          hw_.sustainCoil((uint8_t)activeBit, 100, last_duty);
          continue;
        }
      }

      float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > 0.001f) {
        float fric = params.friction_kinetic * speed;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 4-5. Net force, clamp acceleration
      float accel = sqrtf(fx * fx + fy * fy);
      if (accel > params.target_accel) {
        float scale = params.target_accel / accel;
        fx *= scale;
        fy *= scale;
      }

      // 6. Update velocity
      piece.vx += fx * dt;
      piece.vy += fy * dt;

      // 7. Clamp velocity
      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity) {
        float scale = params.target_velocity / speed;
        piece.vx *= scale;
        piece.vy *= scale;
      }

      // 8. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // 9. Coil switching
      if (coil_idx < path_len - 1) {
        bool passed = moveX
          ? ((dx > 0 && piece.x > cx + 0.01f) || (dx < 0 && piece.x < cx - 0.01f))
          : ((dy > 0 && piece.y > cy + 0.01f) || (dy < 0 && piece.y < cy - 0.01f));

        if (passed) {
          coil_idx++;
          cx = path[coil_idx][0];
          cy = path[coil_idx][1];
          int8_t newBit = coordToBit((uint8_t)cx, (uint8_t)cy);
          if (newBit < 0) return MoveError::COIL_FAILURE;

          activeBit = newBit;
          uint8_t newDuty = forceToDuty(coilForce(0, params), params);
          hw_.pulseBit((uint8_t)activeBit, 1, newDuty);
          last_duty = newDuty;
          prev_sensor_valid = false;

          LOG_BOARD("physics: switch to coil %d at (%d,%d) idx=%d/%d",
                    activeBit, (int)cx, (int)cy, coil_idx, path_len);
        }
      }

      // 10. Set PWM
      duty = forceToDuty(force_mag, params);
      if (abs((int)duty - (int)last_duty) > 2) {
        last_duty = duty;
      }

      // 11. Sensor correction
      if (cal_sensors_) {
        uint8_t si = sensorAt((uint8_t)cx, (uint8_t)cy);
        if (si < cal_count_) {
          uint16_t reading = hw_.readSensor(si);
          float baseline = cal_sensors_[si].baseline_mean;
          float strength = baseline - reading;

          if (strength > params.sensor_threshold) {
            float sensor_dist = sensorDistance(strength, params);
            float scx = ((int)(cx / SR_BLOCK)) * SR_BLOCK;
            float scy = ((int)(cy / SR_BLOCK)) * SR_BLOCK;

            float sensor_pos;
            if (moveX) {
              float dir = (dx > 0) ? -1.0f : 1.0f;
              piece.x = scx + dir * sensor_dist;
              sensor_pos = piece.x;
            } else {
              float dir = (dy > 0) ? -1.0f : 1.0f;
              piece.y = scy + dir * sensor_dist;
              sensor_pos = piece.y;
            }

            if (prev_sensor_valid && dt > 0) {
              float v_sensor = (sensor_pos - prev_sensor_pos) / dt;
              if (moveX) {
                piece.vx = piece.vx * (1.0f - SENSOR_VELOCITY_WEIGHT)
                         + v_sensor * SENSOR_VELOCITY_WEIGHT;
              } else {
                piece.vy = piece.vy * (1.0f - SENSOR_VELOCITY_WEIGHT)
                         + v_sensor * SENSOR_VELOCITY_WEIGHT;
              }
            }
            prev_sensor_pos = sensor_pos;
            prev_sensor_valid = true;
          }
        }
      }

      // 12. Arrival check
      if (coil_idx == path_len - 1 && cal_sensors_) {
        uint8_t si = sensorAt(path[path_len-1][0], path[path_len-1][1]);
        if (si < cal_count_) {
          uint16_t reading = hw_.readSensor(si);
          float piece_mean = cal_sensors_[si].piece_mean;
          if (fabsf(reading - piece_mean) < params.sensor_threshold) {
            LOG_BOARD("physics: arrived at (%d,%d) reading=%d piece_mean=%.0f",
                      path[path_len-1][0], path[path_len-1][1], reading, piece_mean);

            if (coil_idx > 0) {
              float v_along = moveX ? piece.vx : piece.vy;
              float move_dir = moveX ? dx : dy;
              if (v_along * move_dir > 0.1f) {
                int8_t prevBit = coordToBit(path[coil_idx-1][0], path[coil_idx-1][1]);
                if (prevBit >= 0) {
                  hw_.pulseBit((uint8_t)prevBit, 30, 180);
                  LOG_BOARD("physics: brake applied");
                }
              }
            }

            piece.x = path[path_len-1][0];
            piece.y = path[path_len-1][1];
            piece.vx = 0;
            piece.vy = 0;
            piece.stuck = true;

            hw_.pulseBit((uint8_t)activeBit, 1, 0);
            return MoveError::NONE;
          }
        }
      }

      // Sustain coil for next tick
      hw_.sustainCoil((uint8_t)activeBit, 100, last_duty);
    }

    // Timeout
    hw_.pulseBit((uint8_t)activeBit, 1, 0);
    LOG_BOARD("physics: TIMEOUT after %dms pos=(%.1f,%.1f)", params.max_duration_ms, piece.x, piece.y);
    return MoveError::COIL_FAILURE;
  }

private:
  Hardware& hw_;
  const CalSensorData* cal_sensors_ = nullptr;
  int cal_count_ = 0;

  static constexpr uint8_t SR_COLS = 4;
  static constexpr uint8_t SR_ROWS = 3;
  static constexpr uint8_t SR_BLOCK = 3;

  static int8_t coordToBit(uint8_t x, uint8_t y) {
    uint8_t sr_col = x / SR_BLOCK;
    uint8_t sr_row = y / SR_BLOCK;
    if (sr_col >= SR_COLS || sr_row >= SR_ROWS) return -1;
    uint8_t sr_index = sr_col * SR_ROWS + sr_row;
    uint8_t lx = x % SR_BLOCK;
    uint8_t ly = y % SR_BLOCK;
    int8_t local_bit = -1;
    if (ly == 0) local_bit = 2 - lx;
    else if (lx == 0) local_bit = 2 + ly;
    if (local_bit < 0) return -1;
    return (int8_t)(sr_index * 8 + local_bit);
  }

  static uint8_t sensorAt(uint8_t x, uint8_t y) {
    return (x / SR_BLOCK) * SR_ROWS + (SR_ROWS - 1 - (y / SR_BLOCK));
  }

  float coilForce(float d, const PhysicsParams& p) {
    return p.voltage_scale * p.force_k / (powf(d, p.falloff_exp) + p.force_epsilon);
  }

  float maxForce(const PhysicsParams& p) {
    return p.voltage_scale * p.force_k / p.force_epsilon;
  }

  float sensorDistance(float strength, const PhysicsParams& p) {
    if (strength <= 0) return 99.0f;
    float ratio = p.sensor_k / strength;
    if (ratio <= p.force_epsilon) return 0.0f;
    return powf(ratio - p.force_epsilon, 1.0f / p.sensor_falloff);
  }

  uint8_t forceToDuty(float force, const PhysicsParams& p) {
    float mf = maxForce(p);
    if (mf <= 0) return 255;
    float ratio = force / mf;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    return (uint8_t)(ratio * 255.0f);
  }
};
