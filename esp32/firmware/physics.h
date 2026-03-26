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
  // Lateral force model: F = voltage_scale * force_k * d / (d^falloff_exp + epsilon)
  // Zero at center (d=0), peaks at intermediate distance, falls off at large d
  float force_k          = 10.0f;
  float force_epsilon    = 0.3f;
  float falloff_exp      = 2.0f;
  float voltage_scale    = 1.0f;

  // Friction
  float friction_static  = 3.0f;   // force required to start moving
  float friction_kinetic = 2.0f;  // damping force per unit velocity

  // Control targets
  float target_velocity  = 5.0f;   // grid-units/sec
  float target_accel     = 20.0f;  // grid-units/sec^2

  // Sensor distance model (same shape as force, inverted)
  float sensor_k         = 500.0f;
  float sensor_falloff   = 2.0f;
  float sensor_threshold = 50.0f;  // min (baseline - reading) to activate

  // Manual sensor calibration (used when no calibration data available)
  float manual_baseline   = 2030.0f;  // typical ADC reading with no piece
  float manual_piece_mean = 1700.0f;  // typical ADC reading with piece centered

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

    uint8_t duty = 255;
    float force_mag = 0;
    if (!hw_.startCoil((uint8_t)activeBit, duty)) return MoveError::COIL_FAILURE;

    unsigned long t0 = millis();
    unsigned long last_tick_us = micros();

    float prev_sensor_pos = 0;
    bool prev_sensor_valid = false;
    uint8_t last_duty = duty;

    // Stats tracking
    float max_speed = 0;
    float max_force = 0;
    int coil_switches = 0;
    int sensor_corrections = 0;
    bool braked = false;

    LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%d,%d)",
              path_len, piece.x, piece.y, path[path_len-1][0], path[path_len-1][1]);

    while (millis() - t0 < params.max_duration_ms) {
      // 1. Measure dt
      unsigned long now_us = micros();
      float dt = (now_us - last_tick_us) / 1000000.0f;
      last_tick_us = now_us;
      if (dt <= 0 || dt > 0.1f) dt = 0.001f;

      // 2. Compute distance and direction to active coil
      float ddx = cx - piece.x;
      float ddy = cy - piece.y;
      float dist = sqrtf(ddx * ddx + ddy * ddy);
      float dir_x = 0, dir_y = 0;
      if (dist > 0.001f) { dir_x = ddx / dist; dir_y = ddy / dist; }

      // Max force available at this distance (full duty)
      float f_available = coilForce(dist, params);

      // 3. Static friction check (at full duty)
      if (piece.stuck) {
        if (f_available > params.friction_static) {
          piece.stuck = false;
          last_tick_us = micros();
          LOG_BOARD("physics: static friction overcome (F=%.2f > %.2f)", f_available, params.friction_static);
        } else {
          if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
            hw_.stopCoil((uint8_t)activeBit);
            return MoveError::COIL_FAILURE;
          }
          continue;
        }
      }

      // 4. Compute desired force along movement axis
      // Use velocity projected onto movement direction, not magnitude
      float v_along = (moveX ? piece.vx : piece.vy) * (moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f));
      float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);

      // speed_error: positive means we need to go faster in movement direction
      float speed_error = params.target_velocity - v_along;
      float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel), params.target_accel);

      // Desired coil force = desired_accel + friction compensation
      float friction_force = (speed > 0.001f) ? params.friction_kinetic * speed : 0;
      float desired_force = desired_accel + friction_force;
      if (desired_force < 0) desired_force = 0;

      // 5. Compute duty to produce desired force
      if (f_available > 0.001f) {
        float duty_f = (desired_force / f_available) * 255.0f;
        if (duty_f > 255.0f) duty_f = 255.0f;
        if (duty_f < 0.0f) duty_f = 0.0f;
        duty = (uint8_t)duty_f;
      } else {
        duty = 255; // at coil center, force model gives 0 — just coast
      }

      // 6. Actual force applied = available * duty/255
      float actual_force = f_available * (duty / 255.0f);
      force_mag = actual_force;

      // Apply coil force in direction of coil
      float fx = actual_force * dir_x;
      float fy = actual_force * dir_y;

      // Apply friction (opposes velocity, can't reverse)
      if (speed > 0.001f) {
        float fric = params.friction_kinetic * speed;
        float max_fric = speed / dt;
        if (fric > max_fric) fric = max_fric;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 7. Update velocity
      piece.vx += fx * dt;
      piece.vy += fy * dt;

      // Clamp velocity (safety)
      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity * 1.5f) {
        float scale = params.target_velocity * 1.5f / speed;
        piece.vx *= scale;
        piece.vy *= scale;
      }

      // Track stats
      if (speed > max_speed) max_speed = speed;
      if (force_mag > max_force) max_force = force_mag;

      // 8. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // Debug: log every 10ms
      unsigned long elapsed = millis() - t0;
      if (elapsed / 10 != (elapsed - 1) / 10) {
        LOG_BOARD("physics: t=%lums pos=(%.2f,%.2f) v=(%.2f,%.2f) d=%.2f duty=%d",
                  elapsed, piece.x, piece.y, piece.vx, piece.vy, dist, duty);
      }

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

          hw_.stopCoil((uint8_t)activeBit);
          activeBit = newBit;
          hw_.startCoil((uint8_t)activeBit, 255);
          last_duty = 255;
          prev_sensor_valid = false;

          coil_switches++;
          LOG_BOARD("physics: switch to coil %d at (%d,%d) idx=%d/%d",
                    activeBit, (int)cx, (int)cy, coil_idx, path_len);
        }
      }

      // 10. Update PWM
      if (abs((int)duty - (int)last_duty) > 2) {
        hw_.sustainCoil((uint8_t)activeBit, 0, duty);
        last_duty = duty;
      }

      // 11. Sensor correction — for whichever sensor block the active coil is in
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
            float dest_x = path[path_len-1][0];
            float dest_y = path[path_len-1][1];

            // Save sim state before correction
            float sim_pos = moveX ? piece.x : piece.y;
            float sim_vel = moveX ? piece.vx : piece.vy;

            // Correct position: sensor says piece is sensor_dist from center.
            // Only apply if it moves piece closer to center (sensor is more accurate near center).
            float sensor_pos;
            bool corrected_applied = false;
            if (moveX) {
              float sim_dist = fabsf(piece.x - scx);
              if (sensor_dist < sim_dist) {
                float dir = (piece.x >= scx) ? 1.0f : -1.0f;
                piece.x = scx + dir * sensor_dist;
                corrected_applied = true;
              }
              sensor_pos = piece.x;
            } else {
              float sim_dist = fabsf(piece.y - scy);
              if (sensor_dist < sim_dist) {
                float dir = (piece.y >= scy) ? 1.0f : -1.0f;
                piece.y = scy + dir * sensor_dist;
                corrected_applied = true;
              }
              sensor_pos = piece.y;
            }

            float pos_error = sensor_pos - sim_pos;

            float v_sensor = 0;
            if (prev_sensor_valid && dt > 0) {
              v_sensor = (sensor_pos - prev_sensor_pos) / dt;
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
            if (corrected_applied) sensor_corrections++;

            // Log sensor correction on first hit or when error is significant
            if (sensor_corrections == 1 || fabsf(pos_error) > 0.1f || (sensor_corrections % 50 == 0)) {
              LOG_BOARD("physics: sensor s%d t=%lums adc=%d pos: sim=%.2f->cor=%.2f (err=%.2f) vel: sim=%.2f sensor=%.2f",
                        si, millis() - t0, reading, sim_pos, sensor_pos, pos_error, sim_vel, v_sensor);
            }
          }
        }
      }

      // 12. Arrival check — on last coil, sensor shows piece centered
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
                  braked = true;
                  LOG_BOARD("physics: brake applied");
                }
              }
            }

            piece.x = path[path_len-1][0];
            piece.y = path[path_len-1][1];
            piece.vx = 0;
            piece.vy = 0;
            piece.stuck = true;

            hw_.stopCoil((uint8_t)activeBit);
            unsigned long total_ms = millis() - t0;
            float dist_total = sqrtf(dx*dx + dy*dy);
            float avg_speed = (total_ms > 0) ? dist_total / (total_ms / 1000.0f) : 0;
            LOG_BOARD("physics: === MOVE COMPLETE ===");
            LOG_BOARD("physics: time=%lums dist=%.1fgu avg_speed=%.1fgu/s max_speed=%.1fgu/s", total_ms, dist_total, avg_speed, max_speed);
            LOG_BOARD("physics: coil_switches=%d sensor_corrections=%d braked=%d", coil_switches, sensor_corrections, braked ? 1 : 0);
            if (max_speed > params.target_velocity * 0.95f) LOG_BOARD("physics: hint: hit velocity cap, increase target_velocity for faster moves");
            if (max_speed < params.target_velocity * 0.5f) LOG_BOARD("physics: hint: never reached half max speed, increase force_k or reduce friction_kinetic");
            if (total_ms > 2000) LOG_BOARD("physics: hint: slow move, increase force_k or target_accel");
            if (braked) LOG_BOARD("physics: hint: braking needed, increase friction_kinetic to slow naturally");
            if (sensor_corrections == 0) LOG_BOARD("physics: hint: no sensor corrections, check sensor_threshold or calibration");
            if (avg_speed < 1.0f) LOG_BOARD("physics: hint: very slow avg speed, friction may be too high relative to force");
            return MoveError::NONE;
          }
        }
      }

      // Sustain coil for next tick
      if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
        LOG_BOARD("physics: sustainCoil failed, aborting");
        hw_.stopCoil((uint8_t)activeBit);
        return MoveError::COIL_FAILURE;
      }
    }

    // Timeout
    hw_.stopCoil((uint8_t)activeBit);
    float dist_moved = sqrtf((piece.x - path[0][0]) * (piece.x - path[0][0]) + (piece.y - path[0][1]) * (piece.y - path[0][1]));
    LOG_BOARD("physics: === MOVE TIMEOUT ===");
    LOG_BOARD("physics: pos=(%.1f,%.1f) v=(%.1f,%.1f) max_speed=%.1f coil_idx=%d/%d",
              piece.x, piece.y, piece.vx, piece.vy, max_speed, coil_idx, path_len);
    if (max_speed < 0.1f) LOG_BOARD("physics: hint: piece barely moved, increase force_k or reduce friction_static");
    if (coil_idx == 0) LOG_BOARD("physics: hint: never switched coils, piece stuck at start");
    if (sensor_corrections == 0) LOG_BOARD("physics: hint: no sensor data, check sensor_threshold or calibration");
    if (coil_idx == path_len - 1) LOG_BOARD("physics: hint: reached last coil but sensor didn't detect arrival, reduce sensor_threshold");
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

  // Lateral force: zero at center, peaks at intermediate d, falls off at large d
  float coilForce(float d, const PhysicsParams& p) {
    return p.voltage_scale * p.force_k * d / (powf(d, p.falloff_exp) + p.force_epsilon);
  }

  // Peak force occurs at d where d/(d^exp + eps) is maximized
  float maxForce(const PhysicsParams& p) {
    // For exp=2: peak at d=sqrt(eps), value = 1/(2*sqrt(eps))
    // General approx: evaluate at d = eps^(1/exp)
    float d_peak = powf(p.force_epsilon, 1.0f / p.falloff_exp);
    return coilForce(d_peak, p);
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
