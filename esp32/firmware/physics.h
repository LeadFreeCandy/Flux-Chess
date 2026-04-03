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
  uint8_t tick_ms = 10;            // physics tick period in ms (affects coil buzz frequency)
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

    int tick_count = 0;

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

        // Centering pulse: fire when piece has nearly stopped
        // If near destination (<3mm): normal centering
        // If stopped far from destination: sim/reality mismatch, pulse to pull piece in
        float dest_x = path_mm[path_len-1][0];
        float dest_y = path_mm[path_len-1][1];
        float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));

        bool should_pulse = !centered && params.brake_pulse_ms > 0 && speed < 20.0f
                            && (d_dest < COAST_TOLERANCE_MM || speed < 0.5f);
        if (should_pulse) {
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
        // Floor duty to 1 only when accelerating — when decelerating (desired_force reduced
        // by negative desired_accel), duty=0 is correct (let friction slow the piece)
        if (duty == 0 && desired_accel > 0 && desired_force > 0) duty = 1;
        float eff_duty = (duty > 0) ? duty + (255.0f - duty) * comp : 0;
        float actual_current = (eff_duty / 255.0f) * params.max_current_a;
        last_current = actual_current;

        fx = fx_1a * actual_current;
        fy = fy_1a * actual_current;
      }

      // 8. Apply friction (always opposes velocity, can't reverse)
      if (speed > 0.1f) {
        float mu_fric = coasting ? (params.mu_kinetic + params.coast_friction_offset) : params.mu_kinetic;
        float fric = mu_fric * fmaxf(normal_mN, 0);
        float max_fric = speed / dt * mass_kg;  // max force that stops piece in one tick (mN)
        if (fric > max_fric) fric = max_fric;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 9. Update velocity: a = F/m
      // Clamp acceleration during powered movement only — friction spikes from
      // Fz-enhanced normal force can produce unrealistic deceleration near coil centers.
      // During coast, friction is gravity-only and must not be clamped.
      float ax = fx / mass_kg;
      float ay = fy / mass_kg;
      if (!coasting) {
        float accel_mag = sqrtf(ax * ax + ay * ay);
        if (accel_mag > params.target_accel_mm_s2 * 3.0f) {
          float scale = params.target_accel_mm_s2 * 3.0f / accel_mag;
          ax *= scale; ay *= scale;
        }
      }
      piece.vx += ax * dt;
      piece.vy += ay * dt;

      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity_mm_s * SPEED_CLAMP_FACTOR) {
        float scale = params.target_velocity_mm_s * SPEED_CLAMP_FACTOR / speed;
        piece.vx *= scale; piece.vy *= scale;
      }
      if (speed > max_speed) max_speed = speed;

      // 10. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // Debug log every 10 ticks (~10ms at 1kHz)
      tick_count++;
      if (tick_count % 10 == 0) {
        unsigned long elapsed = millis() - t0;
        LOG_BOARD("physics: t=%lums pos=(%.1f,%.1f) v=(%.1f,%.1f) a=(%.0f,%.0f) duty=%d %s",
                  elapsed, piece.x, piece.y, piece.vx, piece.vy, ax, ay, duty,
                  coasting ? (braked ? "BRAKE" : "COAST") : "");
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

  // ── Multi-move: simultaneous piece movement ──────────────
  // Discrete on/off per coil — no PWM. OE stays low (enabled).
  // Each tick: decide per-move whether coil is on or off,
  // combine all active bits into one SR write, hold for tick_ms.

  static constexpr int MAX_SIMULTANEOUS_MOVES = 4;

  // A coil group: a set of coil bits activated together (pairs for diagonal, single for orthogonal)
  static constexpr int MAX_COIL_GROUP_SIZE = 5;
  struct CoilGroup {
    int8_t bits[MAX_COIL_GROUP_SIZE];
    int layers[MAX_COIL_GROUP_SIZE];
    int count = 0;
  };

  // A path of coil groups — each step activates one group
  static constexpr int MAX_GROUP_PATH = 12;

  struct MoveSlot {
    bool active = false;
    PieceState* piece = nullptr;
    bool diagonal = false;

    // Orthogonal: path_mm waypoints, single-coil switching
    const float (*path_mm)[2] = nullptr;
    int path_len = 0;
    int coil_idx = 0;
    float cx, cy;
    float dx, dy;
    bool moveX;
    float move_sign;
    int8_t activeBit = -1;
    int activeLayer = 0;

    // Diagonal: coil group path
    CoilGroup groups[MAX_GROUP_PATH];
    int num_groups = 0;
    int group_idx = 0;

    // Common state
    bool coil_on = false;
    bool coasting = false;
    bool centered = false;
    bool arrived = false;
    MoveError error = MoveError::COIL_FAILURE;
    int coil_switches = 0;
    float max_speed = 0;

    // Destination (for coast/arrival)
    float dest_x, dest_y;
    float dir_x, dir_y;   // unit vector toward destination
  };

  MoveSlot slots_[MAX_SIMULTANEOUS_MOVES];
  int num_queued_ = 0;

  void clearQueue() {
    for (int i = 0; i < MAX_SIMULTANEOUS_MOVES; i++) slots_[i].active = false;
    num_queued_ = 0;
  }

  // Queue an orthogonal move
  bool queueMove(PieceState& piece, const float path_mm[][2], int path_len) {
    if (num_queued_ >= MAX_SIMULTANEOUS_MOVES || path_len < 1) return false;
    MoveSlot& s = slots_[num_queued_];
    s = MoveSlot{}; // reset
    s.active = true;
    s.diagonal = false;
    s.piece = &piece;
    s.path_mm = path_mm;
    s.path_len = path_len;
    s.coil_idx = 0;
    s.cx = path_mm[0][0];
    s.cy = path_mm[0][1];
    s.dx = path_mm[path_len-1][0] - piece.x;
    s.dy = path_mm[path_len-1][1] - piece.y;
    s.moveX = (fabsf(s.dx) > fabsf(s.dy));
    s.move_sign = s.moveX ? (s.dx > 0 ? 1.0f : -1.0f) : (s.dy > 0 ? 1.0f : -1.0f);
    s.activeBit = coordToBit((uint8_t)(s.cx / GRID_TO_MM + 0.5f),
                              (uint8_t)(s.cy / GRID_TO_MM + 0.5f));
    s.activeLayer = 0;
    s.dest_x = path_mm[path_len-1][0];
    s.dest_y = path_mm[path_len-1][1];
    float dist = sqrtf(s.dx * s.dx + s.dy * s.dy);
    s.dir_x = dist > 0.1f ? s.dx / dist : 0;
    s.dir_y = dist > 0.1f ? s.dy / dist : 0;
    num_queued_++;
    return true;
  }

  // Queue a diagonal move — builds coil group path from grid coordinates
  bool queueDiagonalMove(PieceState& piece,
                          uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    if (num_queued_ >= MAX_SIMULTANEOUS_MOVES) return false;
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    if (stepX == 0 || stepY == 0) return false; // not diagonal

    MoveSlot& s = slots_[num_queued_];
    s = MoveSlot{}; // reset
    s.active = true;
    s.diagonal = true;
    s.piece = &piece;
    s.dest_x = toX * GRID_TO_MM;
    s.dest_y = toY * GRID_TO_MM;
    s.dx = s.dest_x - piece.x;
    s.dy = s.dest_y - piece.y;
    float dist = sqrtf(s.dx * s.dx + s.dy * s.dy);
    s.dir_x = dist > 0.1f ? s.dx / dist : 0;
    s.dir_y = dist > 0.1f ? s.dy / dist : 0;

    // Build coil groups for the diagonal path
    // Group 1: catapult pair — (fromX+stepX, fromY) + (fromX, fromY+stepY)
    //          plus mid-range — (fromX+2*stepX, fromY) + (fromX, fromY+2*stepY)
    s.num_groups = 0;
    {
      CoilGroup& g = s.groups[s.num_groups];
      g.count = 0;
      int8_t b1 = coordToBit(fromX + stepX, fromY);
      int8_t b2 = coordToBit(fromX, fromY + stepY);
      if (b1 >= 0) { g.bits[g.count] = b1; g.layers[g.count] = 0; g.count++; }
      if (b2 >= 0) { g.bits[g.count] = b2; g.layers[g.count] = 0; g.count++; }
      int8_t b3 = coordToBit(fromX + stepX * 2, fromY);
      int8_t b4 = coordToBit(fromX, fromY + stepY * 2);
      if (b3 >= 0) { g.bits[g.count] = b3; g.layers[g.count] = 0; g.count++; }
      if (b4 >= 0) { g.bits[g.count] = b4; g.layers[g.count] = 0; g.count++; }
      if (g.count > 0) s.num_groups++;
    }

    // Group 2: catch pairs — (toX, fromY+stepY)+(fromX+stepX, toY)
    //          and (toX, fromY+2*stepY)+(fromX+2*stepX, toY) and (toX,toY)
    {
      CoilGroup& g = s.groups[s.num_groups];
      g.count = 0;
      int8_t b1 = coordToBit(toX, fromY + stepY);
      int8_t b2 = coordToBit(fromX + stepX, toY);
      int8_t b3 = coordToBit(toX, fromY + stepY * 2);
      int8_t b4 = coordToBit(fromX + stepX * 2, toY);
      int8_t b5 = coordToBit(toX, toY);
      if (b1 >= 0) { g.bits[g.count] = b1; g.layers[g.count] = 0; g.count++; }
      if (b2 >= 0) { g.bits[g.count] = b2; g.layers[g.count] = 0; g.count++; }
      if (b3 >= 0) { g.bits[g.count] = b3; g.layers[g.count] = 0; g.count++; }
      if (b4 >= 0) { g.bits[g.count] = b4; g.layers[g.count] = 0; g.count++; }
      if (b5 >= 0 && g.count < MAX_COIL_GROUP_SIZE) { g.bits[g.count] = b5; g.layers[g.count] = 0; g.count++; }
      if (g.count > 0) s.num_groups++;
    }

    // Group 3: center — just (toX, toY)
    {
      CoilGroup& g = s.groups[s.num_groups];
      g.count = 0;
      int8_t b = coordToBit(toX, toY);
      if (b >= 0) { g.bits[g.count] = b; g.layers[g.count] = 0; g.count++; }
      if (g.count > 0) s.num_groups++;
    }

    s.group_idx = 0;
    num_queued_++;
    return true;
  }

  // Execute all queued moves simultaneously. Returns when all are done or timeout.
  void executeMulti(const PhysicsParams& params) {
    if (num_queued_ == 0) return;

    float weight_mN = params.piece_mass_g * 9.81f;
    float mass_kg = params.piece_mass_g * 1e-3f;
    float dt = params.tick_ms * 0.001f;

    static constexpr float COAST_TOLERANCE_MM = 3.0f;
    static constexpr float ARRIVAL_DIST_MM = 1.0f;
    static constexpr float ARRIVAL_SPEED_MM_S = 5.0f;
    static constexpr float CENTERED_SPEED_MM_S = 10.0f;
    static constexpr float SPEED_CLAMP_FACTOR = 1.5f;

    // Unstick all pieces
    for (int i = 0; i < num_queued_; i++) {
      MoveSlot& s = slots_[i];
      if (!s.diagonal && s.activeBit < 0) { s.arrived = true; s.error = MoveError::COIL_FAILURE; continue; }
      if (s.diagonal && s.num_groups == 0) { s.arrived = true; s.error = MoveError::COIL_FAILURE; continue; }
      s.piece->stuck = false;
    }

    unsigned long t0 = millis();
    int tick_count = 0;

    LOG_BOARD("physics: multi-move start, %d moves, tick=%dms", num_queued_, params.tick_ms);

    while (millis() - t0 < params.max_duration_ms) {
      // Check if all moves are done
      bool all_done = true;
      for (int i = 0; i < num_queued_; i++) {
        if (slots_[i].active && !slots_[i].arrived) { all_done = false; break; }
      }
      if (all_done) break;

      // ── Per-move tick logic ──
      for (int i = 0; i < num_queued_; i++) {
        MoveSlot& s = slots_[i];
        if (!s.active || s.arrived) continue;
        PieceState& p = *s.piece;

        float speed = sqrtf(p.vx * p.vx + p.vy * p.vy);
        float v_along = p.vx * s.dir_x + p.vy * s.dir_y;

        // Distance to destination
        float to_dx = s.dest_x - p.x, to_dy = s.dest_y - p.y;
        float dist_remain = sqrtf(to_dx * to_dx + to_dy * to_dy);

        // Coast check
        if (!s.coasting) {
          float coast_mu = params.mu_kinetic + params.coast_friction_offset;
          float friction_decel = coast_mu * weight_mN / mass_kg;
          float stopping_dist = (friction_decel > 0.01f) ? (v_along * v_along) / (2.0f * friction_decel) : 9999.0f;
          if (v_along > 0 && dist_remain > 0 && stopping_dist >= dist_remain) {
            s.coasting = true;
            s.coil_on = false;
            LOG_BOARD("physics: move %d coast v=%.1f stop=%.1f remain=%.1f", i, v_along, stopping_dist, dist_remain);
          }
        }

        float fx = 0, fy = 0;
        float normal_mN = weight_mN;

        if (s.coasting) {
          s.coil_on = false;
          // Centering pulse
          if (!s.centered && params.brake_pulse_ms > 0 &&
              (dist_remain < COAST_TOLERANCE_MM || speed < 0.5f) && speed < 20.0f) {
            int8_t destBit = coordToBit((uint8_t)(s.dest_x / GRID_TO_MM + 0.5f),
                                        (uint8_t)(s.dest_y / GRID_TO_MM + 0.5f));
            if (destBit >= 0) {
              hw_.pulseBit((uint8_t)destBit, params.brake_pulse_ms, 255);
              s.centered = true;
              LOG_BOARD("physics: move %d center pulse %dms", i, params.brake_pulse_ms);
            }
          }
        } else if (s.diagonal) {
          // ── Diagonal: compute combined force from current group's coils ──
          s.coil_on = (v_along < params.target_velocity_mm_s);
          if (s.coil_on && s.group_idx < s.num_groups) {
            const CoilGroup& g = s.groups[s.group_idx];
            for (int c = 0; c < g.count; c++) {
              float coff_x = p.x - (float)((g.bits[c] / 8 / SR_ROWS) * SR_BLOCK) * GRID_TO_MM;
              float coff_y = p.y - (float)((g.bits[c] / 8 % SR_ROWS) * SR_BLOCK) * GRID_TO_MM;
              // Reconstruct coil position from bit index
              uint8_t sr_idx = g.bits[c] / 8;
              uint8_t local_bit = g.bits[c] % 8;
              uint8_t sr_col = sr_idx / SR_ROWS;
              uint8_t sr_row = sr_idx % SR_ROWS;
              float coil_x, coil_y;
              if (local_bit <= 2) { coil_x = (sr_col * SR_BLOCK + (2 - local_bit)) * GRID_TO_MM; coil_y = sr_row * SR_BLOCK * GRID_TO_MM; }
              else { coil_x = sr_col * SR_BLOCK * GRID_TO_MM; coil_y = (sr_row * SR_BLOCK + (local_bit - 2)) * GRID_TO_MM; }

              float dx = p.x - coil_x, dy = p.y - coil_y;
              fx += tableForceFx(0, dx, dy) * params.force_scale * params.max_current_a;
              fy += tableForceFy(0, dx, dy) * params.force_scale * params.max_current_a;
              float fz = tableForceFz(0, dx, dy) * params.force_scale * params.max_current_a;
              normal_mN = fmaxf(normal_mN - fz, normal_mN); // Fz adds to normal
            }
          }

          // Group switching: compare current vs next group net forward accel
          if (!s.coasting && s.group_idx < s.num_groups - 1) {
            float cur_fwd = (fx * s.dir_x + fy * s.dir_y);
            float cur_fric = params.mu_kinetic * normal_mN;
            float cur_net = cur_fwd - cur_fric;

            // Compute next group force
            float nfx = 0, nfy = 0, n_normal = weight_mN;
            const CoilGroup& ng = s.groups[s.group_idx + 1];
            for (int c = 0; c < ng.count; c++) {
              uint8_t sr_idx = ng.bits[c] / 8;
              uint8_t local_bit = ng.bits[c] % 8;
              uint8_t sr_col = sr_idx / SR_ROWS;
              uint8_t sr_row = sr_idx % SR_ROWS;
              float coil_x, coil_y;
              if (local_bit <= 2) { coil_x = (sr_col * SR_BLOCK + (2 - local_bit)) * GRID_TO_MM; coil_y = sr_row * SR_BLOCK * GRID_TO_MM; }
              else { coil_x = sr_col * SR_BLOCK * GRID_TO_MM; coil_y = (sr_row * SR_BLOCK + (local_bit - 2)) * GRID_TO_MM; }

              float dx = p.x - coil_x, dy = p.y - coil_y;
              nfx += tableForceFx(0, dx, dy) * params.force_scale * params.max_current_a;
              nfy += tableForceFy(0, dx, dy) * params.force_scale * params.max_current_a;
              float fz = tableForceFz(0, dx, dy) * params.force_scale * params.max_current_a;
              n_normal = fmaxf(n_normal - fz, n_normal);
            }
            float next_fwd = nfx * s.dir_x + nfy * s.dir_y;
            float next_net = next_fwd - params.mu_kinetic * n_normal;

            if (next_net > cur_net) {
              s.group_idx++;
              s.coil_switches++;
              LOG_BOARD("physics: move %d switch to group %d", i, s.group_idx);
            }
          }
        } else {
          // ── Orthogonal: single coil ──
          float off_x = p.x - s.cx, off_y = p.y - s.cy;
          float fx_1a = tableForceFx(s.activeLayer, off_x, off_y) * params.force_scale;
          float fy_1a = tableForceFy(s.activeLayer, off_x, off_y) * params.force_scale;
          float fz_1a = tableForceFz(s.activeLayer, off_x, off_y) * params.force_scale;

          s.coil_on = (v_along < params.target_velocity_mm_s);
          if (s.coil_on) {
            normal_mN = fmaxf(weight_mN - fz_1a * params.max_current_a, 0.0f);
            fx = fx_1a * params.max_current_a;
            fy = fy_1a * params.max_current_a;
          }

          // Coil switching (net-accel comparison)
          if (!s.coasting && s.coil_idx < s.path_len - 1) {
            float cur_fx_move = s.moveX ? fx_1a : fy_1a;
            float cur_fwd = cur_fx_move * s.move_sign * params.max_current_a;
            float cur_norm = fmaxf(weight_mN - fz_1a * params.max_current_a, 0.0f);
            float cur_net = cur_fwd - params.mu_kinetic * cur_norm;

            float next_cx = s.path_mm[s.coil_idx + 1][0];
            float next_cy = s.path_mm[s.coil_idx + 1][1];
            float n_off_x = p.x - next_cx, n_off_y = p.y - next_cy;
            float nfx = tableForceFx(0, n_off_x, n_off_y) * params.force_scale;
            float nfy = tableForceFy(0, n_off_x, n_off_y) * params.force_scale;
            float nfz = tableForceFz(0, n_off_x, n_off_y) * params.force_scale;
            float next_fx_move = s.moveX ? nfx : nfy;
            float next_fwd = next_fx_move * s.move_sign * params.max_current_a;
            float next_norm = fmaxf(weight_mN - nfz * params.max_current_a, 0.0f);
            float next_net = next_fwd - params.mu_kinetic * next_norm;

            if (next_net > cur_net) {
              s.coil_idx++;
              s.cx = next_cx; s.cy = next_cy;
              s.activeBit = coordToBit((uint8_t)(s.cx / GRID_TO_MM + 0.5f),
                                        (uint8_t)(s.cy / GRID_TO_MM + 0.5f));
              s.activeLayer = 0;
              s.coil_switches++;
            }
          }
        }

        // Friction
        float mu_fric = s.coasting ? (params.mu_kinetic + params.coast_friction_offset) : params.mu_kinetic;
        if (speed > 0.1f) {
          float fric = mu_fric * fmaxf(normal_mN, 0);
          float max_fric = speed / dt * mass_kg;
          if (fric > max_fric) fric = max_fric;
          fx -= fric * (p.vx / speed);
          fy -= fric * (p.vy / speed);
        }

        // Integrate
        float ax = fx / mass_kg, ay = fy / mass_kg;
        if (!s.coasting) {
          float amag = sqrtf(ax * ax + ay * ay);
          if (amag > params.target_accel_mm_s2 * 3.0f) {
            float sc = params.target_accel_mm_s2 * 3.0f / amag;
            ax *= sc; ay *= sc;
          }
        }
        p.vx += ax * dt;
        p.vy += ay * dt;

        speed = sqrtf(p.vx * p.vx + p.vy * p.vy);
        if (speed > params.target_velocity_mm_s * SPEED_CLAMP_FACTOR) {
          float sc = params.target_velocity_mm_s * SPEED_CLAMP_FACTOR / speed;
          p.vx *= sc; p.vy *= sc;
        }
        if (speed > s.max_speed) s.max_speed = speed;

        p.x += p.vx * dt;
        p.y += p.vy * dt;

        // Arrival check
        float d_dest = sqrtf((p.x-s.dest_x)*(p.x-s.dest_x) + (p.y-s.dest_y)*(p.y-s.dest_y));
        bool arrived = (s.centered && speed < CENTERED_SPEED_MM_S) ||
                       (d_dest < ARRIVAL_DIST_MM && speed < ARRIVAL_SPEED_MM_S);
        if (arrived) {
          p.x = s.dest_x; p.y = s.dest_y;
          p.vx = 0; p.vy = 0;
          p.stuck = true;
          s.arrived = true;
          s.error = MoveError::NONE;
          s.coil_on = false;
          unsigned long t = millis() - t0;
          LOG_BOARD("physics: move %d ARRIVED t=%lums max_v=%.0f switches=%d",
                    i, t, s.max_speed, s.coil_switches);
        }
      }

      // ── Combine all active coil bits into one SR write ──
      uint8_t active_bits[MAX_SIMULTANEOUS_MOVES * MAX_COIL_GROUP_SIZE];
      int n_active = 0;
      for (int i = 0; i < num_queued_; i++) {
        const MoveSlot& s = slots_[i];
        if (!s.active || s.arrived || !s.coil_on) continue;
        if (s.diagonal) {
          // Add all bits from current coil group
          if (s.group_idx < s.num_groups) {
            const CoilGroup& g = s.groups[s.group_idx];
            for (int c = 0; c < g.count && n_active < (int)sizeof(active_bits); c++) {
              active_bits[n_active++] = (uint8_t)g.bits[c];
            }
          }
        } else {
          if (s.activeBit >= 0) active_bits[n_active++] = (uint8_t)s.activeBit;
        }
      }

      if (n_active > 0) {
        hw_.startCoils(active_bits, n_active, 255);
      } else {
        hw_.stopAllCoils();
      }

      // Hold for tick duration
      delay(params.tick_ms);

      // Log every 10 ticks
      tick_count++;
      if (tick_count % 10 == 0) {
        unsigned long elapsed = millis() - t0;
        for (int i = 0; i < num_queued_; i++) {
          MoveSlot& s = slots_[i];
          if (!s.active || s.arrived) continue;
          LOG_BOARD("physics: t=%lums m%d pos=(%.1f,%.1f) v=(%.1f,%.1f) %s%s",
                    elapsed, i, s.piece->x, s.piece->y, s.piece->vx, s.piece->vy,
                    s.coil_on ? "ON" : "off", s.coasting ? " COAST" : "");
        }
      }
    }

    // Cleanup: stop all coils
    hw_.stopAllCoils();

    // Report any moves that didn't arrive
    for (int i = 0; i < num_queued_; i++) {
      if (slots_[i].active && !slots_[i].arrived) {
        LOG_BOARD("physics: move %d TIMEOUT pos=(%.1f,%.1f) v=(%.1f,%.1f)",
                  i, slots_[i].piece->x, slots_[i].piece->y,
                  slots_[i].piece->vx, slots_[i].piece->vy);
      }
    }
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
