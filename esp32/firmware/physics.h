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
  float piece_mass_g       = 2.7f;
  float max_current_a      = 1.0f;
  float mu_static          = 0.35f;
  float mu_kinetic         = 0.25f;
  float target_velocity_mm_s = 100.0f;
  float target_accel_mm_s2   = 500.0f;
  float max_jerk_mm_s3       = 50000.0f; // rate of change of acceleration (mm/s³)
  float coast_friction_offset = 0.0f; // added to mu_kinetic for coast distance calculation only
  uint16_t brake_pulse_ms  = 100;   // 0=no centering pulse, >0=pulse duration at destination
  uint16_t pwm_freq_hz     = 20000;
  float pwm_compensation   = 0.2f;  // 0=ideal PWM, 1=always full on. Accounts for MOSFET gate RC
  bool  all_coils_equal    = false; // ignore layer differences, use layer 0 (closest) for all
  float force_scale        = 1.0f;  // multiplier on force table values (tune sim vs reality gap)
  uint16_t max_duration_ms = 5000;
  uint8_t max_retry_attempts = 0;  // 0=no checkpoint recovery, >0=retry with moveDumb on failure
};


class PhysicsMove {
public:
  PhysicsMove(Hardware& hw) : hw_(hw) {
    LOG_BOARD("physics: force tables loaded (%d layers, %dx%d)",
              FORCE_TABLE_NUM_LAYERS, FORCE_TABLE_SIZE, FORCE_TABLE_SIZE);
  }

  // Sensor correction disabled — pure physics sim for now

  MoveError execute(PieceState& piece, const float path_mm[][2], int path_len,
                    const PhysicsParams& params,
                    MoveDiag* diag = nullptr,
                    const uint8_t* path_sensors = nullptr, int num_path_sensors = 0,
                    const float* sensor_thresholds = nullptr) {
    if (path_len < 1) return MoveError::COIL_FAILURE;
    if (params.piece_mass_g <= 0 || params.max_current_a <= 0 || params.target_velocity_mm_s <= 0) {
      LOG_BOARD("physics: invalid params (mass=%.1f current=%.1f vel=%.1f)",
                params.piece_mass_g, params.max_current_a, params.target_velocity_mm_s);
      return MoveError::COIL_FAILURE;
    }

    int coil_idx = 0;
    float cx = path_mm[0][0], cy = path_mm[0][1];

    float dx = path_mm[path_len - 1][0] - piece.x;
    float dy = path_mm[path_len - 1][1] - piece.y;
    bool moveX = (fabsf(dx) > fabsf(dy));
    float move_sign = moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f);

    int8_t activeBit = coordToBit((uint8_t)(path_mm[0][0] / GRID_TO_MM + 0.5f),
                                   (uint8_t)(path_mm[0][1] / GRID_TO_MM + 0.5f));
    if (activeBit < 0) return MoveError::COIL_FAILURE;
    int activeLayer = params.all_coils_equal ? 0 : bitToLayer(activeBit);

    float weight_mN = params.piece_mass_g * 9.81f;
    float mass_kg = params.piece_mass_g * 1e-3f;

    // Set PWM frequency from params
    hw_.setPwmFrequency(params.pwm_freq_hz);

    if (!hw_.startCoil((uint8_t)activeBit, 255)) return MoveError::COIL_FAILURE;

    unsigned long t0 = millis();
    unsigned long last_tick_us = micros();
    uint8_t last_duty = 255;
    float last_current = params.max_current_a;

    // Stats
    float max_speed = 0;
    int coil_switches = 0;
    bool braked = false;
    bool coasting = false;       // true after coils cut for coast-to-stop
    bool centered = false;       // true after centering pulse fired
    // Simulation constants
    static constexpr float COAST_TOLERANCE_MM = 3.0f;    // trigger centering pulse within this
    static constexpr float ARRIVAL_DIST_MM = 1.0f;       // consider arrived when closer than this
    static constexpr float ARRIVAL_SPEED_MM_S = 5.0f;    // and slower than this
    static constexpr float CENTERED_SPEED_MM_S = 10.0f;  // after centering pulse, done when below this
    static constexpr float SPEED_CLAMP_FACTOR = 1.5f;    // clamp velocity to target * this

    unsigned long last_log_ms = 0;

    // Sensor diagnostic tracking
    if (diag) {
      diag->num_coils = (uint8_t)fminf(path_len, MAX_DIAG_COILS);
      for (int i = 0; i < diag->num_coils; i++) {
        diag->coils[i].sensor_idx = (path_sensors && i < num_path_sensors) ? path_sensors[i] : 0;
        diag->coils[i].min_reading = 0xFFFF;
        diag->coils[i].detected = false;
        diag->coils[i].arrival_reading = 0;
      }
    }

    LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%.1f,%.1f) mm",
              path_len, piece.x, piece.y, path_mm[path_len-1][0], path_mm[path_len-1][1]);

    while (millis() - t0 < params.max_duration_ms) {
      // 1. dt — enforce minimum 1ms tick (1kHz)
      unsigned long now_us = micros();
      unsigned long elapsed_us = now_us - last_tick_us;
      if (elapsed_us < 1000) {
        unsigned long deadline_us = last_tick_us + 1000;
        // Sample path sensors as fast as possible in the dead time
        while (micros() < deadline_us) {
          if (diag && path_sensors && num_path_sensors > 0) {
            for (int si = 0; si < diag->num_coils && si < num_path_sensors; si++) {
              uint16_t r = hw_.readSensor(path_sensors[si]);
              if (r < diag->coils[si].min_reading) diag->coils[si].min_reading = r;
              if (sensor_thresholds && r < (uint16_t)sensor_thresholds[si]) diag->coils[si].detected = true;
            }
          } else {
            delayMicroseconds(100);  // no sensors to sample, just wait
          }
        }
        now_us = micros();
        elapsed_us = now_us - last_tick_us;
      }
      float dt = elapsed_us / 1000000.0f;
      last_tick_us = now_us;
      if (dt > 0.005f) dt = 0.005f;  // cap at 5ms to prevent huge jumps

      // 2. Offset from active coil in mm
      float off_x = piece.x - cx;
      float off_y = piece.y - cy;

      // 3. Force table lookup (mN at 1A)
      float fx_1a = tableForceFx(activeLayer, off_x, off_y) * params.force_scale;
      float fy_1a = tableForceFy(activeLayer, off_x, off_y) * params.force_scale;
      float fz_1a = tableForceFz(activeLayer, off_x, off_y) * params.force_scale;

      float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      float v_along = (moveX ? piece.vx : piece.vy) * move_sign;

      // 4. Dynamic friction — Fz is negative (pulls magnet toward coil/PCB),
      // which INCREASES normal force (piece pressed harder into surface).
      float normal_mN = weight_mN - fz_1a * last_current;
      if (normal_mN < 0) normal_mN = 0;
      float mu = (piece.stuck || speed < 0.1f) ? params.mu_static : params.mu_kinetic;
      float friction_mN = mu * normal_mN;

      // 5. Static friction check
      if (piece.stuck) {
        float avail = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a) * params.max_current_a;
        float static_fric = params.mu_static * (weight_mN - fz_1a * params.max_current_a);
        if (avail > fmaxf(static_fric, 0)) {
          piece.stuck = false;
          last_tick_us = micros();
          LOG_BOARD("physics: unstuck (F=%.1f > fric=%.1f mN)", avail, static_fric);
        } else {
          if (!hw_.sustainCoil((uint8_t)activeBit, 0, last_duty)) {
            hw_.stopCoil((uint8_t)activeBit);
            return MoveError::COIL_FAILURE;
          }
          continue;
        }
      }

      float fx, fy;
      uint8_t duty;

      if (!coasting) {
        // 6. Stopping check — can we coast to destination from here?
        float dest_x = path_mm[path_len-1][0];
        float dest_y = path_mm[path_len-1][1];
        float dist_remain = moveX ? (dest_x - piece.x) * move_sign : (dest_y - piece.y) * move_sign;

        // Friction-only deceleration for coast decision (offset allows tuning)
        float coast_mu = params.mu_kinetic + params.coast_friction_offset;
        float friction_decel = (weight_mN > 0) ? coast_mu * weight_mN / mass_kg : 0;
        float stopping_dist = (friction_decel > 0.01f) ? (v_along * v_along) / (2.0f * friction_decel) : 9999.0f;

        if (v_along > 0 && dist_remain > 0 && stopping_dist >= dist_remain) {
          // Cut all coils — coast to stop on friction alone
          coasting = true;
          hw_.stopCoil((uint8_t)activeBit);
          last_current = 0;  // no coil active — prevents Fz from zeroing normal force in friction calc
          normal_mN = weight_mN;  // recompute: no Fz contribution with coils off
          LOG_BOARD("physics: coast start v=%.1f stop_dist=%.1f dist_remain=%.1f",
                    v_along, stopping_dist, dist_remain);
        }
      }

      if (coasting) {
        // No coil force during coast — friction only
        fx = 0; fy = 0;
        duty = 0;

        // Centering pulse: when piece has nearly stopped near destination, pulse dest coil
        float dest_x = path_mm[path_len-1][0];
        float dest_y = path_mm[path_len-1][1];
        float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));

        if (!centered && params.brake_pulse_ms > 0 && d_dest < COAST_TOLERANCE_MM && speed < 20.0f) {
          // Fire centering pulse on destination coil
          int8_t destBit = coordToBit((uint8_t)(dest_x / GRID_TO_MM + 0.5f),
                                      (uint8_t)(dest_y / GRID_TO_MM + 0.5f));
          if (destBit >= 0) {
            hw_.pulseBit((uint8_t)destBit, params.brake_pulse_ms, 255);
            centered = true;
            braked = true;
            LOG_BOARD("physics: centering pulse %dms at d=%.1fmm v=%.1f",
                      params.brake_pulse_ms, d_dest, speed);
          }
        }
      } else {
        // 7. Controller: desired force → current → duty
        float speed_error = params.target_velocity_mm_s - v_along;
        float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel_mm_s2), params.target_accel_mm_s2);

        if (desired_accel < 0) {
          LOG_BOARD("physics: WARNING negative accel requested: %.1f mm/s^2 (v=%.1f target=%.1f)",
                    desired_accel, v_along, params.target_velocity_mm_s);
        }

        float desired_force = mass_kg * desired_accel + friction_mN;
        if (desired_force < 0) desired_force = 0;  // never actively brake, let friction handle it

        float avail_lateral = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a);
        float required_current = (avail_lateral > 0.01f) ? desired_force / avail_lateral : params.max_current_a;
        required_current = fminf(required_current, params.max_current_a);
        if (required_current < 0) required_current = 0;

        // Compute raw duty accounting for PWM compensation
        // effective = raw + (255 - raw) * comp → raw = (effective - 255*comp) / (1 - comp)
        float desired_eff_duty = required_current / params.max_current_a * 255.0f;
        float comp = params.pwm_compensation;
        float raw_duty = (comp < 0.99f) ? (desired_eff_duty - 255.0f * comp) / (1.0f - comp) : 0;
        if (raw_duty < 0) raw_duty = 0;
        if (raw_duty > 255) raw_duty = 255;
        duty = (uint8_t)raw_duty;
        if (duty == 0 && desired_force > 0) duty = 1;
        float eff_duty = (duty > 0) ? duty + (255.0f - duty) * comp : 0;
        float actual_current = (eff_duty / 255.0f) * params.max_current_a;
        last_current = actual_current;

        fx = fx_1a * actual_current;
        fy = fy_1a * actual_current;
      }

      // 8. Apply friction (always opposes velocity, can't reverse)
      if (speed > 0.1f) {
        float fric = params.mu_kinetic * fmaxf(normal_mN, 0);
        float max_fric = speed / dt * mass_kg;  // max force that stops piece in one tick (mN)
        if (fric > max_fric) fric = max_fric;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 9. Update velocity: a = F/m
      piece.vx += (fx / mass_kg) * dt;
      piece.vy += (fy / mass_kg) * dt;

      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity_mm_s * SPEED_CLAMP_FACTOR) {
        float scale = params.target_velocity_mm_s * SPEED_CLAMP_FACTOR / speed;
        piece.vx *= scale; piece.vy *= scale;
      }
      if (speed > max_speed) max_speed = speed;

      // 10. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // Debug log every 10ms (every 10th tick at 1kHz)
      {
        unsigned long elapsed = millis() - t0;
        if (elapsed != last_log_ms) {
          last_log_ms = elapsed;
          if (elapsed % 10 == 0) {
            LOG_BOARD("physics: t=%lums pos=(%.1f,%.1f) v=(%.1f,%.1f) duty=%d %s",
                      elapsed, piece.x, piece.y, piece.vx, piece.vy, duty,
                      coasting ? (braked ? "BRAKE" : "COAST") : "");
          }
        }
      }

      // 11. Coil switching — switch when next coil produces greater forward acceleration
      if (!coasting && coil_idx < path_len - 1) {
        float next_cx = path_mm[coil_idx + 1][0];
        float next_cy = path_mm[coil_idx + 1][1];
        float next_off_x = piece.x - next_cx;
        float next_off_y = piece.y - next_cy;

        int nextLayer = params.all_coils_equal ? 0 : bitToLayer(
          coordToBit((uint8_t)(next_cx / GRID_TO_MM + 0.5f), (uint8_t)(next_cy / GRID_TO_MM + 0.5f)));

        // Current coil: forward acceleration at max current
        float cur_fx_move = moveX ? fx_1a : fy_1a;
        float cur_forward = cur_fx_move * move_sign * params.max_current_a;
        float cur_normal = fmaxf(weight_mN - fz_1a * params.max_current_a, 0.0f);
        float cur_net = cur_forward - params.mu_kinetic * cur_normal;

        // Next coil: forward acceleration at max current
        float nfx_1a = tableForceFx(nextLayer, next_off_x, next_off_y) * params.force_scale;
        float nfy_1a = tableForceFy(nextLayer, next_off_x, next_off_y) * params.force_scale;
        float nfz_1a = tableForceFz(nextLayer, next_off_x, next_off_y) * params.force_scale;
        float next_fx_move = moveX ? nfx_1a : nfy_1a;
        float next_forward = next_fx_move * move_sign * params.max_current_a;
        float next_normal = fmaxf(weight_mN - nfz_1a * params.max_current_a, 0.0f);
        float next_net = next_forward - params.mu_kinetic * next_normal;

        if (next_net > cur_net) {
          coil_idx++;
          cx = next_cx;
          cy = next_cy;

          uint8_t gx = (uint8_t)(cx / GRID_TO_MM + 0.5f);
          uint8_t gy = (uint8_t)(cy / GRID_TO_MM + 0.5f);
          int8_t newBit = coordToBit(gx, gy);
          if (newBit < 0) return MoveError::COIL_FAILURE;

          hw_.stopCoil((uint8_t)activeBit);
          activeBit = newBit;
          activeLayer = params.all_coils_equal ? 0 : bitToLayer(newBit);
          hw_.startCoil((uint8_t)activeBit, 255);
          last_duty = 255;
          last_current = params.max_current_a;
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
      {
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));

      // After centering pulse, or if close enough and stopped
      bool arrived = (centered && speed < CENTERED_SPEED_MM_S) || (d_dest < ARRIVAL_DIST_MM && speed < ARRIVAL_SPEED_MM_S);

      if (arrived) {
        piece.x = dest_x; piece.y = dest_y;
        piece.vx = 0; piece.vy = 0;
        piece.stuck = true;

        if (!coasting) hw_.stopCoil((uint8_t)activeBit);

        unsigned long total_ms = millis() - t0;
        float dist_total = sqrtf(dx*dx + dy*dy);
        float avg_speed = (total_ms > 0) ? dist_total / (total_ms / 1000.0f) : 0;
        LOG_BOARD("physics: === MOVE COMPLETE ===");
        LOG_BOARD("physics: time=%lums dist=%.1fmm avg=%.0fmm/s max=%.0fmm/s switches=%d braked=%d",
                  total_ms, dist_total, avg_speed, max_speed, coil_switches, braked ? 1 : 0);
        // Read final sensor values for diagnostics
        if (diag && path_sensors && num_path_sensors > 0) {
          for (int i = 0; i < diag->num_coils && i < num_path_sensors; i++) {
            diag->coils[i].arrival_reading = hw_.readSensor(path_sensors[i]);
          }
        }
        return MoveError::NONE;
      }
      }

      // Update PWM (coil stays on between ticks)
      if (!coasting) {
        if (!hw_.sustainCoil((uint8_t)activeBit, 0, last_duty)) {
          hw_.stopCoil((uint8_t)activeBit);
          return MoveError::COIL_FAILURE;
        }
      }
    }

    // Timeout
    if (!coasting) hw_.stopCoil((uint8_t)activeBit);
    LOG_BOARD("physics: TIMEOUT pos=(%.1f,%.1f) v=(%.1f,%.1f) max_speed=%.0f",
              piece.x, piece.y, piece.vx, piece.vy, max_speed);
    // Read final sensor values on timeout too
    if (diag && path_sensors && num_path_sensors > 0) {
      for (int i = 0; i < diag->num_coils && i < num_path_sensors; i++) {
        diag->coils[i].arrival_reading = hw_.readSensor(path_sensors[i]);
      }
    }
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

  // bit 0=(2,0)→L3, bit 1=(1,0)→L4, bit 2=(0,0)→L0, bit 3=(0,1)→L2, bit 4=(0,2)→L1
  static constexpr int BIT_TO_LAYER[] = {3, 4, 0, 2, 1};

  static int bitToLayer(uint8_t global_bit) {
    uint8_t local = global_bit % 8;
    return (local < 5) ? BIT_TO_LAYER[local] : 0;
  }

  // Force table lookup (bilinear interpolation)
  float tableForceFx(int layer, float dx_mm, float dy_mm) {
    if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
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
    if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
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
    if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
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
