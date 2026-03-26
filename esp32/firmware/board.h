// Board is the game-logic layer. It must NOT directly manipulate shift registers,
// OE, or PWM. All coil actuation goes through Hardware's safe public API:
//   pulseBit()      — fixed-duration pulse with thermal protection
//   sustainCoil()   — sustain active coil without SPI writes
// Hardware encapsulates all dangerous SR/OE/PWM operations as private.

#pragma once
#include "api.h"
#include "hardware.h"
#include "physics.h"

// Piece IDs
#define PIECE_NONE  0
#define PIECE_WHITE 1
#define PIECE_BLACK 2

// Move parameters
#define MOVE_PULSE_MS       350
#define MOVE_DELAY_MS       0
#define MOVE_PULSE_REPEATS  1

class Board {
public:
  Board() {
    initDefaultBoard();
  }

  // ── Piece State ─────────────────────────────────────────────

  uint8_t getPiece(uint8_t x, uint8_t y) const {
    if (x >= GRID_COLS || y >= GRID_ROWS) return PIECE_NONE;
    return pieces_[x][y];
  }

  void setPiece(uint8_t x, uint8_t y, uint8_t id) {
    if (x >= GRID_COLS || y >= GRID_ROWS) return;
    pieces_[x][y] = id;
  }

  // ── Dumb Orthogonal Move ────────────────────────────────────
  // Validates the move, then pulses coils along the path.

  MoveError moveDumbOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY, bool skipValidation = false) {
    LOG_BOARD("moveDumbOrthogonal: (%d,%d) -> (%d,%d) skip=%d", fromX, fromY, toX, toY, skipValidation);

    // Bounds check always applies
    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS) {
      return MoveError::OUT_OF_BOUNDS;
    }

    if (fromX == toX && fromY == toY) {
      return MoveError::SAME_POSITION;
    }

    if (fromX != toX && fromY != toY) {
      return MoveError::NOT_ORTHOGONAL;
    }

    if (!skipValidation) {
      if (getPiece(fromX, fromY) == PIECE_NONE) {
        return MoveError::NO_PIECE_AT_SOURCE;
      }

      int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
      int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
      int8_t cx = fromX + dx, cy = fromY + dy;
      while (cx != toX || cy != toY) {
        if (getPiece(cx, cy) != PIECE_NONE) {
          LOG_BOARD("moveDumbOrthogonal REJECT: path blocked at (%d,%d)", cx, cy);
          return MoveError::PATH_BLOCKED;
        }
        cx += dx;
        cy += dy;
      }
    }

    // Pulse coils along the path
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    uint8_t piece = pieces_[fromX][fromY];
    int8_t cx = fromX, cy = fromY;

    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;

      int8_t bit = coordToBit(cx, cy);
      LOG_BOARD("moveDumbOrthogonal: pulsing (%d,%d) bit=%d for %dms", cx, cy, bit, MOVE_PULSE_MS);
      for (int r = 0; r < MOVE_PULSE_REPEATS; r++) {
        PulseCoilResponse res = pulseCoil(cx, cy, MOVE_PULSE_MS);
        if (!res.success) {
          LOG_BOARD("moveDumbOrthogonal ABORT: pulse failed at (%d,%d), error=%d", cx, cy, (int)res.error);
          return MoveError::COIL_FAILURE;
        }
        if (r < MOVE_PULSE_REPEATS - 1) delay(MOVE_DELAY_MS);
      }
      delay(MOVE_DELAY_MS);
    }

    // Update piece state
    pieces_[fromX][fromY] = PIECE_NONE;
    pieces_[toX][toY] = piece;
    LOG_BOARD("moveDumbOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
    return MoveError::NONE;
  }

  // ── Physics Orthogonal Move ──────────────────────────────────

  MoveError movePhysicsOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                  const PhysicsParams& params = PhysicsParams{}, bool skipValidation = false) {
    LOG_BOARD("movePhysicsOrthogonal: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS)
      return MoveError::OUT_OF_BOUNDS;
    if (fromX == toX && fromY == toY) return MoveError::SAME_POSITION;
    if (fromX != toX && fromY != toY) return MoveError::NOT_ORTHOGONAL;

    if (!skipValidation) {
      if (getPiece(fromX, fromY) == PIECE_NONE) return MoveError::NO_PIECE_AT_SOURCE;
      int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
      int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
      int8_t cx = fromX + dx, cy = fromY + dy;
      while (cx != toX || cy != toY) {
        if (getPiece(cx, cy) != PIECE_NONE) return MoveError::PATH_BLOCKED;
        cx += dx; cy += dy;
      }
    }

    // Build coil path
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    uint8_t path[GRID_COLS + GRID_ROWS][2];
    int path_len = 0;
    int8_t cx = fromX, cy = fromY;
    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;
      path[path_len][0] = cx;
      path[path_len][1] = cy;
      path_len++;
    }

    // Provide calibration data to physics engine
    if (cal_data_.valid) {
      updatePhysicsCalData();
    } else {
      // Use manual baseline/piece_mean for all sensors
      for (int i = 0; i < NUM_HALL_SENSORS; i++) {
        cal_sensor_data_[i].baseline_mean = params.manual_baseline;
        cal_sensor_data_[i].piece_mean = params.manual_piece_mean;
      }
      physics_.setCalData(cal_sensor_data_, NUM_HALL_SENSORS);
    }

    // Execute physics move
    PieceState& ps = piece_states_[fromX][fromY];
    ps.reset(fromX, fromY);
    MoveError err = physics_.execute(ps, path, path_len, params);

    if (err == MoveError::NONE) {
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[toX][toY] = piece;
      piece_states_[toX][toY] = ps;
      LOG_BOARD("movePhysicsOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
    } else {
      LOG_BOARD("movePhysicsOrthogonal FAILED: %d", (int)err);
    }
    return err;
  }

  // ── Physics Tuning ────────────────────────────────────────────
  // Automated 4-phase tuning: sensor fit, static friction, force profile, verification.
  // Place piece at (3,3) before calling.

  static constexpr int TUNE_NUM_REPS = 5;
  static constexpr int TUNE_FRICTION_PULSE_MS = 100;
  static constexpr int TUNE_FRICTION_DUTY_STEP = 10;
  static constexpr int TUNE_SETTLE_MS = 50;
  static constexpr uint8_t TUNE_X = 3;
  static constexpr uint8_t TUNE_Y = 3;
  static constexpr int TUNE_DURATIONS[] = {10, 25, 50, 100, 200};
  static constexpr int TUNE_NUM_DURATIONS = 5;

  String tunePhysics() {
    LOG_BOARD("TUNE: start — expecting piece at (%d,%d)", TUNE_X, TUNE_Y);

    memset(pieces_, PIECE_NONE, sizeof(pieces_));
    pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;

    uint8_t si = sensorForGrid(TUNE_X, TUNE_Y);
    int8_t coil_bit = coordToBit(TUNE_X + 1, TUNE_Y);  // adjacent coil for testing

    // Get baseline
    float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2030.0f;

    // ── Phase 1: Sensor curve fitting ──
    LOG_BOARD("TUNE: phase1 - sensor curve fitting");
    uint16_t readings_d0[TUNE_NUM_REPS];
    uint16_t readings_d1[TUNE_NUM_REPS];
    uint16_t readings_d2[TUNE_NUM_REPS];

    // d=0: piece at sensor center
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      readings_d0[i] = hw_.readSensor(si);
      delay(10);
    }

    // d=1: move piece one unit away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 1, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d1[i] = hw_.readSensor(si);
      calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // d=2: move piece two units away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d2[i] = hw_.readSensor(si);
      calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // Sort arrays for median
    tuneSort(readings_d0, TUNE_NUM_REPS);
    tuneSort(readings_d1, TUNE_NUM_REPS);
    tuneSort(readings_d2, TUNE_NUM_REPS);

    float str0 = baseline - readings_d0[TUNE_NUM_REPS / 2];
    float str1 = baseline - readings_d1[TUNE_NUM_REPS / 2];
    float str2 = baseline - readings_d2[TUNE_NUM_REPS / 2];

    // Fit sensor_k and sensor_falloff
    float eps = 0.3f;  // use force_epsilon as shared epsilon
    float fitted_sensor_k = (str0 > 0) ? str0 * eps : 500.0f;
    float fitted_sensor_falloff = 2.0f;  // default
    if (str1 > 1.0f && str2 > 1.0f) {
      // Solve: str1/str2 = (2^falloff + eps) / (1 + eps)
      float ratio = str1 / str2;
      // Approximate: try falloff values and pick best fit
      float best_err = 999;
      for (float f = 1.0f; f <= 4.0f; f += 0.1f) {
        float predicted_ratio = (powf(2.0f, f) + eps) / (1.0f + eps);
        float err = fabsf(predicted_ratio - ratio);
        if (err < best_err) { best_err = err; fitted_sensor_falloff = f; }
      }
    }

    LOG_BOARD("TUNE: sensor fit: k=%.1f falloff=%.2f (str: d0=%.0f d1=%.0f d2=%.0f)",
              fitted_sensor_k, fitted_sensor_falloff, str0, str1, str2);

    // ── Phase 2: Static friction measurement ──
    LOG_BOARD("TUNE: phase2 - static friction");
    uint8_t threshold_duties[TUNE_NUM_REPS];

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Ensure piece is centered
      calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);  // no-op if already there
      delay(TUNE_SETTLE_MS);
      uint16_t pre = hw_.readSensor(si);

      uint8_t found_duty = 255;
      for (int d = TUNE_FRICTION_DUTY_STEP; d <= 255; d += TUNE_FRICTION_DUTY_STEP) {
        hw_.pulseBit((uint8_t)coil_bit, TUNE_FRICTION_PULSE_MS, (uint8_t)d);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        if (abs((int)post - (int)pre) > 20) {
          found_duty = (uint8_t)d;
          LOG_BOARD("TUNE: friction rep %d: moved at duty=%d (pre=%d post=%d)", rep, d, pre, post);
          break;
        }

        // Re-center for next duty step
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);  // pull back if it drifted
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        pieces_[TUNE_X + 1][TUNE_Y] = PIECE_NONE;
        delay(TUNE_SETTLE_MS);
        pre = hw_.readSensor(si);  // re-baseline
      }

      threshold_duties[rep] = found_duty;

      // Re-center piece
      // Find where piece is and move back
      for (int x = 0; x < GRID_COLS; x++)
        for (int y = 0; y < GRID_ROWS; y++)
          if (pieces_[x][y] != PIECE_NONE && (x != TUNE_X || y != TUNE_Y)) {
            calMove(x, y, TUNE_X, TUNE_Y);
          }
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    tuneSort(threshold_duties, TUNE_NUM_REPS);
    uint8_t median_duty = threshold_duties[TUNE_NUM_REPS / 2];

    // Convert to friction_static using force model at d=1.0
    float default_force_k = 10.0f;
    float force_at_1 = default_force_k * 1.0f / (powf(1.0f, 2.0f) + eps);
    float fitted_friction_static = force_at_1 * (median_duty / 255.0f);

    LOG_BOARD("TUNE: static friction: median_duty=%d friction_static=%.2f", median_duty, fitted_friction_static);

    // ── Phase 3: Coil force profiling ──
    LOG_BOARD("TUNE: phase3 - force profiling");
    float displacements[TUNE_NUM_DURATIONS];

    for (int di = 0; di < TUNE_NUM_DURATIONS; di++) {
      float disp_sum = 0;
      int valid_reps = 0;

      for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
        // Ensure centered
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);
        delay(TUNE_SETTLE_MS);

        uint16_t pre = hw_.readSensor(si);
        hw_.pulseBit((uint8_t)coil_bit, TUNE_DURATIONS[di], 255);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        float strength_pre = baseline - pre;
        float strength_post = baseline - post;

        // Estimate displacement using sensor model
        float d_pre = (strength_pre > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_pre - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 0.0f;
        float d_post = (strength_post > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_post - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 3.0f;

        float disp = d_post - d_pre;
        if (disp > 0) {
          disp_sum += disp;
          valid_reps++;
        }

        LOG_BOARD("TUNE: force dur=%dms rep=%d pre=%d post=%d disp=%.2f",
                  TUNE_DURATIONS[di], rep, pre, post, disp);

        // Re-center
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
        calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      }

      displacements[di] = (valid_reps > 0) ? disp_sum / valid_reps : 0;
      LOG_BOARD("TUNE: force dur=%dms avg_disp=%.3f", TUNE_DURATIONS[di], displacements[di]);
    }

    // Fit force_k: at d=1, F = force_k * 1/(1+eps).
    // displacement ≈ 0.5 * (F - friction_static) * t^2 for short pulses
    // Use the 100ms data point for fitting
    float t_fit = 0.1f;  // 100ms
    float disp_fit = displacements[3];  // index 3 = 100ms
    float fitted_force_k = default_force_k;
    if (disp_fit > 0.01f) {
      // disp = 0.5 * (F - fric_s) * t^2, F = fk * 1/(1+eps)
      // fk = (2*disp/t^2 + fric_s) * (1+eps)
      fitted_force_k = (2.0f * disp_fit / (t_fit * t_fit) + fitted_friction_static) * (1.0f + eps);
    }

    // Fit friction_kinetic from ratio of displacements
    // Longer pulses show diminishing returns due to friction limiting speed
    float fitted_friction_kinetic = 2.0f;  // default
    if (displacements[4] > 0.01f && displacements[2] > 0.01f) {
      // ratio of 200ms/50ms displacement — if purely kinematic would be 16x, less = more friction
      float ratio = displacements[4] / displacements[2];
      // friction_kinetic ≈ force / terminal_velocity, estimate from saturation
      if (ratio < 10.0f && ratio > 1.0f) {
        float terminal_v = displacements[4] / 0.2f;  // rough v from displacement/time
        float f_net = fitted_force_k * 1.0f / (1.0f + eps);
        fitted_friction_kinetic = f_net / fmaxf(terminal_v, 0.1f);
      }
    }

    LOG_BOARD("TUNE: fitted force_k=%.2f friction_kinetic=%.2f", fitted_force_k, fitted_friction_kinetic);

    // ── Phase 4: Move verification ──
    LOG_BOARD("TUNE: phase4 - verification");
    int success_count = 0;
    uint16_t dest_readings[TUNE_NUM_REPS];
    unsigned long elapsed_ms[TUNE_NUM_REPS];

    PhysicsParams test_params;
    test_params.force_k = fitted_force_k;
    test_params.force_epsilon = eps;
    test_params.friction_static = fitted_friction_static;
    test_params.friction_kinetic = fitted_friction_kinetic;
    test_params.sensor_k = fitted_sensor_k;
    test_params.sensor_falloff = fitted_sensor_falloff;
    test_params.manual_baseline = baseline;
    test_params.manual_piece_mean = readings_d0[TUNE_NUM_REPS / 2];

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Move piece to (5,3)
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      pieces_[TUNE_X][TUNE_Y] = PIECE_NONE;
      pieces_[TUNE_X + 2][TUNE_Y] = PIECE_WHITE;
      delay(TUNE_SETTLE_MS);

      // Attempt physics move back
      unsigned long t0 = millis();
      MoveError err = movePhysicsOrthogonal(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y, test_params, true);
      elapsed_ms[rep] = millis() - t0;

      delay(100);
      dest_readings[rep] = hw_.readSensor(si);

      bool arrived = (err == MoveError::NONE) && (dest_readings[rep] < baseline - 100);
      if (arrived) success_count++;

      LOG_BOARD("TUNE: verify rep=%d err=%d t=%lums reading=%d %s",
                rep, (int)err, elapsed_ms[rep], dest_readings[rep], arrived ? "OK" : "MISS");

      // Re-center
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    LOG_BOARD("TUNE: verification: %d/%d success", success_count, TUNE_NUM_REPS);

    // ── Build result JSON ──
    String j = "{\"sensor_fit\":{\"sensor_k\":";
    j += String(fitted_sensor_k, 1);
    j += ",\"sensor_falloff\":";
    j += String(fitted_sensor_falloff, 2);
    j += ",\"readings_d0\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d0[i]); }
    j += "],\"readings_d1\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d1[i]); }
    j += "],\"readings_d2\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d2[i]); }
    j += "]},\"static_friction\":{\"threshold_duties\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(threshold_duties[i]); }
    j += "],\"median_duty\":";
    j += String(median_duty);
    j += ",\"friction_static\":";
    j += String(fitted_friction_static, 2);
    j += "},\"force_profile\":{\"durations_ms\":[10,25,50,100,200],\"displacements\":[";
    for (int i = 0; i < TUNE_NUM_DURATIONS; i++) { if (i) j += ","; j += String(displacements[i], 3); }
    j += "],\"force_k\":";
    j += String(fitted_force_k, 2);
    j += ",\"friction_kinetic\":";
    j += String(fitted_friction_kinetic, 2);
    j += "},\"verification\":{\"success_count\":";
    j += String(success_count);
    j += ",\"total\":";
    j += String(TUNE_NUM_REPS);
    j += ",\"dest_readings\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(dest_readings[i]); }
    j += "],\"elapsed_ms\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(elapsed_ms[i]); }
    j += "]},\"recommended\":{";
    j += "\"force_k\":"; j += String(fitted_force_k, 2);
    j += ",\"force_epsilon\":"; j += String(eps, 2);
    j += ",\"falloff_exp\":2.0";
    j += ",\"voltage_scale\":1.0";
    j += ",\"friction_static\":"; j += String(fitted_friction_static, 2);
    j += ",\"friction_kinetic\":"; j += String(fitted_friction_kinetic, 2);
    j += ",\"target_velocity\":5.0";
    j += ",\"target_accel\":20.0";
    j += ",\"sensor_k\":"; j += String(fitted_sensor_k, 1);
    j += ",\"sensor_falloff\":"; j += String(fitted_sensor_falloff, 2);
    j += ",\"sensor_threshold\":50.0";
    j += ",\"manual_baseline\":"; j += String(baseline, 1);
    j += ",\"manual_piece_mean\":"; j += String((float)readings_d0[TUNE_NUM_REPS / 2], 1);
    j += ",\"max_duration_ms\":5000";
    j += "}}";

    LOG_BOARD("TUNE: complete");
    initDefaultBoard();
    return j;
  }

  // ── Calibration ─────────────────────────────────────────────

  static constexpr int CAL_BASELINE_SAMPLES = 10000;
  static constexpr int CAL_PIECE_REPEATS = 5;
  static constexpr int CAL_SETTLE_MS = 250;

  CalibrateResponse calibrate() {
    LOG_BOARD("CAL: start");
    memset(&cal_data_, 0, sizeof(cal_data_));
    memset(pieces_, PIECE_NONE, sizeof(pieces_));
    pieces_[0][0] = PIECE_WHITE;

    // Sweep pieces to origin
    if (!calMove(9,6,9,0) || !calMove(6,6,6,0) || !calMove(3,6,3,0) ||
        !calMove(0,6,0,0) || !calMove(9,0,0,0)) return { false };

    // Phase 1: baselines (no piece nearby)
    LOG_BOARD("CAL: baselines - right half (piece at 0,0)");
    calMeasureHalf(2, 3);

    LOG_BOARD("CAL: moving piece (0,0) -> (6,0)");
    if (!calMove(0,0,6,0)) return { false };

    LOG_BOARD("CAL: baselines - left half (piece at 6,0)");
    calMeasureHalf(0, 1);

    // Phase 2: jiggle-measure each sensor with piece
    LOG_BOARD("CAL: phase2 - with piece");
    uint8_t px = 6, py = 0;

    for (int row = 0; row < (int)SR_ROWS; row++) {
      for (int ci = 0; ci < (int)SR_COLS; ci++) {
        int col = (row % 2 == 0) ? ci : ((int)SR_COLS - 1 - ci);
        uint8_t tx = col * SR_BLOCK;
        uint8_t ty = row * SR_BLOCK;

        if (!calMove(px, py, tx, ty)) return { false };
        px = tx; py = ty;

        uint8_t si = col * SR_ROWS + (SR_ROWS - 1 - row);
        auto& cs = cal_data_.sensors[si];
        calJiggleMeasure(tx, ty, si, cs.piece_mean, cs.piece_stddev, cs.piece_median);
        LOG_BOARD("CAL: piece sensor %d (%d,%d): mean=%.1f stddev=%.2f median=%d",
                  si, tx, ty, cs.piece_mean, cs.piece_stddev, cs.piece_median);
      }
    }

    // Phase 3: validation - zig-zag across board
    LOG_BOARD("CAL: phase3 - validation");
    memset(cal_data_.detections, 0, sizeof(cal_data_.detections));

    if (!calMove(px, py, 9, 6)) return { false };
    if (!calValidatePass()) return { false };

    cal_data_.valid = true;
    updatePhysicsCalData();
    LOG_BOARD("CAL: complete");
    initDefaultBoard();
    return { true };
  }

  // ── Coil Control ──────────────────────────────────────────

  PulseCoilResponse pulseCoil(uint8_t x, uint8_t y, uint16_t duration_ms) {
    PulseCoilResponse res = { false, PulseError::NONE };

    if (x >= GRID_COLS || y >= GRID_ROWS) { res.error = PulseError::INVALID_COIL; return res; }
    if (duration_ms > MAX_PULSE_MS) { res.error = PulseError::PULSE_TOO_LONG; return res; }
    int8_t bit = coordToBit(x, y);
    if (bit < 0) { res.error = PulseError::INVALID_COIL; return res; }
    if (!hw_.pulseBit((uint8_t)bit, duration_ms)) { res.error = PulseError::THERMAL_LIMIT; return res; }

    res.success = true;
    return res;
  }

  // ── Board State ───────────────────────────────────────────

  GetBoardStateResponse getBoardState() {
    GetBoardStateResponse res = {};

    uint16_t raw[NUM_HALL_SENSORS];
    hw_.readAllSensors(raw, NUM_HALL_SENSORS);
    // Sensors 0-11 map column-major with inverted rows:
    // sensor i → sr_col = i/3, physical row = (2 - i%3)
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      int col = i / SR_ROWS;
      int row = (SR_ROWS - 1) - (i % SR_ROWS);
      res.raw_strengths[col][row] = raw[i];
    }

    memcpy(res.pieces, pieces_, sizeof(pieces_));
    return res;
  }

  // ── RGB / System ──────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) { hw_.setRGB(r, g, b); return { true }; }
  void shutdown() { hw_.shutdown(); }

  // ── Calibration Data Access ─────────────────────────────────

  struct CalSensor {
    float baseline_mean;
    float baseline_stddev;
    float piece_mean;
    float piece_stddev;
    uint16_t piece_median;
  };

  struct CalData {
    bool valid = false;
    CalSensor sensors[NUM_HALL_SENSORS];
    uint8_t detections[SENSOR_COLS][SENSOR_ROWS]; // 0, 1, or 2 from validation passes
  };

  GetCalibrationResponse getCalibration() {
    return { calDataToJson() };
  }

private:
  Hardware hw_;
  uint8_t pieces_[GRID_COLS][GRID_ROWS];
  CalData cal_data_;
  PieceState piece_states_[GRID_COLS][GRID_ROWS];
  PhysicsMove physics_{hw_};
  CalSensorData cal_sensor_data_[NUM_HALL_SENSORS];

  void updatePhysicsCalData() {
    if (!cal_data_.valid) return;
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal_sensor_data_[i].baseline_mean = cal_data_.sensors[i].baseline_mean;
      cal_sensor_data_[i].piece_mean = cal_data_.sensors[i].piece_mean;
    }
    physics_.setCalData(cal_sensor_data_, NUM_HALL_SENSORS);
  }

  // ── Default Board ───────────────────────────────────────────
  // 3 white on bottom row, 3 black on top row
  // Missing 4th piece on each side (only 3 per side)

  void initDefaultBoard() {
    memset(pieces_, PIECE_NONE, sizeof(pieces_));

    // 3 white at y=0
    pieces_[0][0] = PIECE_WHITE;
    pieces_[3][0] = PIECE_WHITE;
    pieces_[6][0] = PIECE_WHITE;

    // 3 black at y=6
    pieces_[0][6] = PIECE_BLACK;
    pieces_[3][6] = PIECE_BLACK;
    pieces_[6][6] = PIECE_BLACK;

    // Reset physics states
    for (int x = 0; x < GRID_COLS; x++)
      for (int y = 0; y < GRID_ROWS; y++)
        piece_states_[x][y].reset(x, y);

    LOG_BOARD("initDefaultBoard: 3 white at y=0, 3 black at y=6");
  }

  // ── Calibration Helpers ──────────────────────────────────────

  // Baseline: 10k samples, mean + stddev
  void calMeasureBaseline(uint8_t si, float& out_mean, float& out_stddev) {
    double sum = 0, sum_sq = 0;
    for (int s = 0; s < CAL_BASELINE_SAMPLES; s++) {
      uint16_t val = hw_.readSensor(si);
      sum += val;
      sum_sq += (double)val * val;
    }
    out_mean = (float)(sum / CAL_BASELINE_SAMPLES);
    out_stddev = (float)sqrt((sum_sq / CAL_BASELINE_SAMPLES) - (double)out_mean * out_mean);
  }

  // Measure baselines for sensors in the given sr_col range
  void calMeasureHalf(int colMin, int colMax) {
    for (int si = 0; si < NUM_HALL_SENSORS; si++) {
      int col = si / SR_ROWS;
      if (col < colMin || col > colMax) continue;
      int row = (SR_ROWS - 1) - (si % SR_ROWS);

      calMeasureBaseline(si, cal_data_.sensors[si].baseline_mean, cal_data_.sensors[si].baseline_stddev);
      LOG_BOARD("CAL: baseline sensor %d (%d,%d): mean=%.1f stddev=%.2f",
                si, col * SR_BLOCK, row * SR_BLOCK,
                cal_data_.sensors[si].baseline_mean, cal_data_.sensors[si].baseline_stddev);
    }
  }

  // Jiggle-measure: move to position, read, jiggle off and back, read again.
  // Repeat CAL_PIECE_REPEATS times, compute mean + stddev from single readings.
  void calJiggleMeasure(uint8_t x, uint8_t y, uint8_t si,
                        float& out_mean, float& out_stddev, uint16_t& out_median) {
    uint8_t sr_row = y / SR_BLOCK;
    int8_t jdy = (sr_row == SR_ROWS - 1) ? -1 : 1;
    uint8_t jy = (uint8_t)(y + jdy);

    uint16_t vals[CAL_PIECE_REPEATS];
    double sum = 0, sum_sq = 0;

    for (int i = 0; i < CAL_PIECE_REPEATS; i++) {
      calMove(x, y, x, jy);
      calMove(x, jy, x, y);
      delay(CAL_SETTLE_MS);

      vals[i] = hw_.readSensor(si);
      sum += vals[i];
      sum_sq += (double)vals[i] * vals[i];
      LOG_BOARD("CAL: jiggle %d/%d sensor %d val=%d", i + 1, CAL_PIECE_REPEATS, si, vals[i]);
    }

    // Sort for median (insertion sort, tiny array)
    for (int i = 1; i < CAL_PIECE_REPEATS; i++) {
      uint16_t key = vals[i];
      int j = i - 1;
      while (j >= 0 && vals[j] > key) { vals[j + 1] = vals[j]; j--; }
      vals[j + 1] = key;
    }

    out_mean = (float)(sum / CAL_PIECE_REPEATS);
    out_stddev = (float)sqrt((sum_sq / CAL_PIECE_REPEATS) - (double)out_mean * out_mean);
    out_median = vals[CAL_PIECE_REPEATS / 2];
  }

  // Check sensor at position, set detection to 1 if below threshold
  void calCheckSensor(uint8_t x, uint8_t y) {
    uint8_t col = x / SR_BLOCK;
    uint8_t row = y / SR_BLOCK;
    uint8_t si = col * SR_ROWS + (SR_ROWS - 1 - row);
    auto& s = cal_data_.sensors[si];

    /* delay(CAL_SETTLE_MS); */
    uint16_t val = hw_.readSensor(si);
    float threshold = (s.baseline_mean + s.piece_mean) / 2.0f;
    bool detected = (val < threshold);

    if (detected) cal_data_.detections[col][row] = 1;
    LOG_BOARD("CAL: validate (%d,%d) sensor=%d val=%d thresh=%.0f %s",
              x, y, si, val, threshold, detected ? "DETECTED" : "miss");
  }

  // Validation: horizontal zig-zag from top-right to bottom-left
  bool calValidatePass() {
    uint8_t px = 9, py = 6;

    for (int row = (int)SR_ROWS - 1; row >= 0; row--) {
      for (int ci = 0; ci < (int)SR_COLS; ci++) {
        int col = (row % 2 == 0) ? ((int)SR_COLS - 1 - ci) : ci;
        uint8_t tx = col * SR_BLOCK;
        uint8_t ty = row * SR_BLOCK;
        if (tx != px || ty != py) {
          if (!calMove(px, py, tx, ty)) return false;
          px = tx; py = ty;
        }
        calCheckSensor(px, py);
      }
    }
    return true;
  }

  // Map grid position to sensor index (column-major, inverted rows)
  static uint8_t sensorForGrid(uint8_t x, uint8_t y) {
    return (x / SR_BLOCK) * SR_ROWS + (SR_ROWS - 1 - (y / SR_BLOCK));
  }

  // Insertion sort for small arrays (used by tuning)
  static void tuneSort(uint16_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint16_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }

  static void tuneSort(uint8_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint8_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }

  // Wrapper: move with skipValidation, handles diagonals, log on error
  bool calMove(uint8_t fx, uint8_t fy, uint8_t tx, uint8_t ty) {
    if (fx == tx && fy == ty) return true;

    // Diagonal: move x first, then y
    if (fx != tx && fy != ty) {
      if (!calMove(fx, fy, tx, fy)) return false;
      return calMove(tx, fy, tx, ty);
    }

    MoveError err = moveDumbOrthogonal(fx, fy, tx, ty, true);
    if (err != MoveError::NONE) {
      LOG_BOARD("CAL: move (%d,%d)->(%d,%d) failed: %d", fx, fy, tx, ty, (int)err);
    }
    return err == MoveError::NONE;
  }



  // ── Calibration JSON Serialization ──────────────────────────

  String calDataToJson() {
    if (!cal_data_.valid) return Json().add("valid", false).build();

    String j = "{\"valid\":true,\"sensors\":[";
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      if (i) j += ",";
      auto& s = cal_data_.sensors[i];
      j += "{\"baseline_mean\":";
      j += String(s.baseline_mean, 1);
      j += ",\"baseline_stddev\":";
      j += String(s.baseline_stddev, 2);
      j += ",\"piece_mean\":";
      j += String(s.piece_mean, 1);
      j += ",\"piece_stddev\":";
      j += String(s.piece_stddev, 2);
      j += ",\"piece_median\":";
      j += String(s.piece_median);
      j += "}";
    }
    j += "],\"detections\":[";
    for (int col = 0; col < SENSOR_COLS; col++) {
      if (col) j += ",";
      j += "[";
      for (int row = 0; row < SENSOR_ROWS; row++) {
        if (row) j += ",";
        j += String(cal_data_.detections[col][row]);
      }
      j += "]";
    }
    j += "]}";
    return j;
  }

  // ── Coil Grid Mapping ─────────────────────────────────────

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
};
