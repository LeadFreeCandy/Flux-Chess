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
