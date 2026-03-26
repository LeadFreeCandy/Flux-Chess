#pragma once
#include <Arduino.h>
#include <math.h>
#include "hardware.h"
#include "utils.h"
#include "api.h"
#include "force_tables.h"

// Coordinate conversion: 1 grid unit = 38/3 mm = 12.667 mm
static constexpr float GRID_TO_MM = 38.0f / 3.0f;

struct PieceState {
  float x, y;     // mm
  float vx, vy;   // mm/s
  bool stuck;

  void reset(float px_mm, float py_mm) {
    x = px_mm; y = py_mm;
    vx = 0; vy = 0;
    stuck = true;
  }
};

struct PhysicsParams {
  float piece_mass_g       = 4.3f;
  float max_current_a      = 1.0f;
  float mu_static          = 0.35f;
  float mu_kinetic         = 0.25f;
  float target_velocity_mm_s = 100.0f;
  float target_accel_mm_s2   = 500.0f;
  bool  active_brake       = true;
  uint16_t max_duration_ms = 5000;
};


class PhysicsMove {
public:
  PhysicsMove(Hardware& hw) : hw_(hw) {
    LOG_BOARD("physics: force tables loaded (%d layers, %dx%d)",
              FORCE_TABLE_NUM_LAYERS, FORCE_TABLE_SIZE, FORCE_TABLE_SIZE);
  }

  // Sensor correction disabled — pure physics sim for now

  MoveError execute(PieceState& piece, const float path_mm[][2], int path_len,
                    const PhysicsParams& params) {
    if (path_len < 1) return MoveError::COIL_FAILURE;

    int coil_idx = 0;
    float cx = path_mm[0][0], cy = path_mm[0][1];

    float dx = path_mm[path_len - 1][0] - piece.x;
    float dy = path_mm[path_len - 1][1] - piece.y;
    bool moveX = (fabsf(dx) > fabsf(dy));
    float move_sign = moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f);

    int8_t activeBit = coordToBit((uint8_t)(path_mm[0][0] / GRID_TO_MM + 0.5f),
                                   (uint8_t)(path_mm[0][1] / GRID_TO_MM + 0.5f));
    if (activeBit < 0) return MoveError::COIL_FAILURE;
    int activeLayer = bitToLayer(activeBit);

    float weight_mN = params.piece_mass_g * 9.81f;
    float mass_kg = params.piece_mass_g * 1e-3f;

    if (!hw_.startCoil((uint8_t)activeBit, 255)) return MoveError::COIL_FAILURE;

    unsigned long t0 = millis();
    unsigned long last_tick_us = micros();
    uint8_t last_duty = 255;
    float last_current = params.max_current_a;

    // Stats
    float max_speed = 0;
    int coil_switches = 0;
    bool braked = false;
    bool coasting = false;  // true after coil cut for stopping

    LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%.1f,%.1f) mm",
              path_len, piece.x, piece.y, path_mm[path_len-1][0], path_mm[path_len-1][1]);

    while (millis() - t0 < params.max_duration_ms) {
      // 1. dt
      unsigned long now_us = micros();
      float dt = (now_us - last_tick_us) / 1000000.0f;
      last_tick_us = now_us;
      if (dt <= 0 || dt > 0.1f) dt = 0.001f;

      // 2. Offset from active coil in mm
      float off_x = piece.x - cx;
      float off_y = piece.y - cy;

      // 3. Force table lookup (mN at 1A)
      float fx_1a = tableForceFx(activeLayer, off_x, off_y);
      float fy_1a = tableForceFy(activeLayer, off_x, off_y);
      float fz_1a = tableForceFz(activeLayer, off_x, off_y);

      float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      float v_along = (moveX ? piece.vx : piece.vy) * move_sign;

      // 4. Dynamic friction
      float normal_mN = weight_mN + fz_1a * last_current;
      if (normal_mN < 0) normal_mN = 0;
      float mu = (piece.stuck || speed < 0.1f) ? params.mu_static : params.mu_kinetic;
      float friction_mN = mu * normal_mN;

      // 5. Static friction check
      if (piece.stuck) {
        float avail = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a) * params.max_current_a;
        float static_fric = params.mu_static * (weight_mN + fz_1a * params.max_current_a);
        if (avail > fmaxf(static_fric, 0)) {
          piece.stuck = false;
          last_tick_us = micros();
          LOG_BOARD("physics: unstuck (F=%.1f > fric=%.1f mN)", avail, static_fric);
        } else {
          if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
            hw_.stopCoil((uint8_t)activeBit);
            return MoveError::COIL_FAILURE;
          }
          continue;
        }
      }

      float fx, fy;
      uint8_t duty;

      if (!coasting) {
        // 6. Stopping check
        float dest_x = path_mm[path_len-1][0];
        float dest_y = path_mm[path_len-1][1];
        float dist_remain = moveX ? fabsf(dest_x - piece.x) : fabsf(dest_y - piece.y);

        float friction_decel = (weight_mN > 0) ? params.mu_kinetic * weight_mN / mass_kg : 0;
        float brake_decel = friction_decel;

        if (params.active_brake && coil_idx > 0) {
          // Lookup brake force from previous coil
          int8_t prevBit = coordToBit((uint8_t)(path_mm[coil_idx-1][0] / GRID_TO_MM + 0.5f),
                                      (uint8_t)(path_mm[coil_idx-1][1] / GRID_TO_MM + 0.5f));
          if (prevBit >= 0) {
            int prevLayer = bitToLayer(prevBit);
            float prev_cx = path_mm[coil_idx-1][0];
            float prev_cy = path_mm[coil_idx-1][1];
            float bfx = tableForceFx(prevLayer, piece.x - prev_cx, piece.y - prev_cy);
            float bfy = tableForceFy(prevLayer, piece.x - prev_cx, piece.y - prev_cy);
            float brake_force = sqrtf(bfx*bfx + bfy*bfy) * params.max_current_a;
            brake_decel = friction_decel + brake_force / mass_kg;
            brake_decel = fminf(brake_decel, params.target_accel_mm_s2);
          }
        }

        float stopping_dist = (brake_decel > 0.01f) ? (v_along * v_along) / (2.0f * brake_decel) : 9999.0f;

        if (coil_idx == path_len - 1 && v_along > 0 && stopping_dist >= dist_remain) {
          // Time to stop
          coasting = true;
          hw_.stopCoil((uint8_t)activeBit);

          if (params.active_brake && coil_idx > 0) {
            int8_t prevBit = coordToBit((uint8_t)(path_mm[coil_idx-1][0] / GRID_TO_MM + 0.5f),
                                        (uint8_t)(path_mm[coil_idx-1][1] / GRID_TO_MM + 0.5f));
            if (prevBit >= 0) {
              hw_.startCoil((uint8_t)prevBit, 255);
              activeBit = prevBit;
              activeLayer = bitToLayer(prevBit);
              braked = true;
              LOG_BOARD("physics: braking at v=%.1f dist_remain=%.1f stop_dist=%.1f",
                        v_along, dist_remain, stopping_dist);
            }
          } else {
            LOG_BOARD("physics: coasting at v=%.1f dist_remain=%.1f", v_along, dist_remain);
          }
        }
      }

      if (coasting) {
        // Coasting/braking — friction + optional brake coil
        if (braked) {
          fx = tableForceFx(activeLayer, piece.x - cx, piece.y - cy) * params.max_current_a;
          fy = tableForceFy(activeLayer, piece.x - cx, piece.y - cy) * params.max_current_a;
          // cx/cy here is the brake coil, which pulls backwards — that's correct
        } else {
          fx = 0; fy = 0;
        }
        duty = braked ? 255 : 0;
      } else {
        // 7. Controller: desired force → current → duty
        float speed_error = params.target_velocity_mm_s - v_along;
        float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel_mm_s2), params.target_accel_mm_s2);
        float desired_force = mass_kg * desired_accel + friction_mN;
        if (desired_force < 0) desired_force = 0;

        float avail_lateral = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a);
        float required_current = (avail_lateral > 0.01f) ? desired_force / avail_lateral : params.max_current_a;
        required_current = fminf(required_current, params.max_current_a);
        if (required_current < 0) required_current = 0;

        duty = (uint8_t)(required_current / params.max_current_a * 255.0f);
        float actual_current = (duty / 255.0f) * params.max_current_a;
        last_current = actual_current;

        fx = fx_1a * actual_current;
        fy = fy_1a * actual_current;
      }

      // 8. Apply friction (always opposes velocity)
      if (speed > 0.1f) {
        float fric = params.mu_kinetic * fmaxf(normal_mN, 0);
        float max_fric_accel = speed / dt * mass_kg;
        if (fric > max_fric_accel) fric = max_fric_accel;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 9. Update velocity: a = F/m
      piece.vx += (fx / mass_kg) * dt;
      piece.vy += (fy / mass_kg) * dt;

      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity_mm_s * 1.5f) {
        float scale = params.target_velocity_mm_s * 1.5f / speed;
        piece.vx *= scale; piece.vy *= scale;
      }
      if (speed > max_speed) max_speed = speed;

      // 10. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // Debug log every 100ms
      unsigned long elapsed = millis() - t0;
      if (elapsed / 100 != (elapsed - 1) / 100) {
        LOG_BOARD("physics: t=%lums pos=(%.1f,%.1f) v=(%.1f,%.1f) duty=%d %s",
                  elapsed, piece.x, piece.y, piece.vx, piece.vy, duty,
                  coasting ? (braked ? "BRAKE" : "COAST") : "");
      }

      // 11. Coil switching (only while not coasting)
      if (!coasting && coil_idx < path_len - 1) {
        bool passed = moveX
          ? ((dx > 0 && piece.x > cx + 0.1f) || (dx < 0 && piece.x < cx - 0.1f))
          : ((dy > 0 && piece.y > cy + 0.1f) || (dy < 0 && piece.y < cy - 0.1f));

        if (passed) {
          coil_idx++;
          cx = path_mm[coil_idx][0];
          cy = path_mm[coil_idx][1];

          uint8_t gx = (uint8_t)(cx / GRID_TO_MM + 0.5f);
          uint8_t gy = (uint8_t)(cy / GRID_TO_MM + 0.5f);
          int8_t newBit = coordToBit(gx, gy);
          if (newBit < 0) return MoveError::COIL_FAILURE;

          hw_.stopCoil((uint8_t)activeBit);
          activeBit = newBit;
          activeLayer = bitToLayer(newBit);
          hw_.startCoil((uint8_t)activeBit, 255);
          last_duty = 255;
          coil_switches++;

          LOG_BOARD("physics: switch coil %d at (%.1f,%.1f) L%d idx=%d/%d",
                    activeBit, cx, cy, activeLayer, coil_idx, path_len);
        }
      }

      // 12. PWM update
      if (!coasting && abs((int)duty - (int)last_duty) > 2) {
        hw_.sustainCoil((uint8_t)activeBit, 0, duty);
        last_duty = duty;
      }

      // 13. Arrival check
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));

      if (d_dest < 1.0f && speed < 5.0f) {
        piece.x = dest_x; piece.y = dest_y;
        piece.vx = 0; piece.vy = 0;
        piece.stuck = true;

        if (!coasting) hw_.stopCoil((uint8_t)activeBit);
        if (braked) hw_.stopCoil((uint8_t)activeBit);

        unsigned long total_ms = millis() - t0;
        float dist_total = sqrtf(dx*dx + dy*dy);
        float avg_speed = (total_ms > 0) ? dist_total / (total_ms / 1000.0f) : 0;
        LOG_BOARD("physics: === MOVE COMPLETE ===");
        LOG_BOARD("physics: time=%lums dist=%.1fmm avg=%.0fmm/s max=%.0fmm/s switches=%d braked=%d",
                  total_ms, dist_total, avg_speed, max_speed, coil_switches, braked ? 1 : 0);
        return MoveError::NONE;
      }

      // Sustain
      if (!coasting) {
        if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
          hw_.stopCoil((uint8_t)activeBit);
          return MoveError::COIL_FAILURE;
        }
      } else if (braked) {
        hw_.sustainCoil((uint8_t)activeBit, 100, 255);
      } else {
        delayMicroseconds(100);
      }
    }

    // Timeout
    if (!coasting) hw_.stopCoil((uint8_t)activeBit);
    if (braked) hw_.stopCoil((uint8_t)activeBit);
    LOG_BOARD("physics: TIMEOUT pos=(%.1f,%.1f) v=(%.1f,%.1f) max_speed=%.0f",
              piece.x, piece.y, piece.vx, piece.vy, max_speed);
    return MoveError::COIL_FAILURE;
  }

private:
  Hardware& hw_;

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

  static constexpr int BIT_TO_LAYER[] = {2, 1, 0, 3, 4};

  static int bitToLayer(uint8_t global_bit) {
    uint8_t local = global_bit % 8;
    return (local < 5) ? BIT_TO_LAYER[local] : 0;
  }

  // Force table lookup (bilinear interpolation)
  float tableForceFx(int layer, float dx_mm, float dy_mm) {
    float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    int ix = (int)fx, iy = (int)fy;
    if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
    float tx = fx - ix, ty = fy - iy;
    return (1-tx)*(1-ty)*force_table_fx[layer][iy][ix]
         + tx*(1-ty)*force_table_fx[layer][iy][ix+1]
         + (1-tx)*ty*force_table_fx[layer][iy+1][ix]
         + tx*ty*force_table_fx[layer][iy+1][ix+1];
  }

  float tableForceFy(int layer, float dx_mm, float dy_mm) {
    float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    int ix = (int)fx, iy = (int)fy;
    if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
    float tx = fx - ix, ty = fy - iy;
    return (1-tx)*(1-ty)*force_table_fy[layer][iy][ix]
         + tx*(1-ty)*force_table_fy[layer][iy][ix+1]
         + (1-tx)*ty*force_table_fy[layer][iy+1][ix]
         + tx*ty*force_table_fy[layer][iy+1][ix+1];
  }

  // Vertical force (negative = pulls magnet down toward coil, increases friction)
  float tableForceFz(int layer, float dx_mm, float dy_mm) {
    float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
    int ix = (int)fx, iy = (int)fy;
    if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
    float tx = fx - ix, ty = fy - iy;
    return (1-tx)*(1-ty)*force_table_fz[layer][iy][ix]
         + tx*(1-ty)*force_table_fz[layer][iy][ix+1]
         + (1-tx)*ty*force_table_fz[layer][iy+1][ix]
         + tx*ty*force_table_fz[layer][iy+1][ix+1];
  }
};
