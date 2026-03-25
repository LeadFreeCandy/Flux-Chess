#pragma once
#include "api.h"
#include "hardware.h"

// Piece IDs
#define PIECE_NONE  0
#define PIECE_WHITE 1
#define PIECE_BLACK 2

// Pulse duration for each step of a dumb move
#define MOVE_PULSE_MS 250

class Board {
public:
  Board() {
    memset(pieces_, PIECE_NONE, sizeof(pieces_));
  }

  // ── Piece State ─────────────────────────────────────────────

  uint8_t getPiece(uint8_t x, uint8_t y) const {
    if (x >= GRID_COLS || y >= GRID_ROWS) return PIECE_NONE;
    return pieces_[x][y];
  }

  void setPiece(uint8_t x, uint8_t y, uint8_t id) {
    if (x >= GRID_COLS || y >= GRID_ROWS) return;
    pieces_[x][y] = id;
    LOG_BOARD("setPiece: (%d,%d) = %d", x, y, id);
  }

  // ── Dumb Orthogonal Move ────────────────────────────────────
  // Moves a piece along a major axis (both coords must be %3==0,
  // and either x's or y's must match). Pulses each coil along
  // the path for MOVE_PULSE_MS each.

  bool moveDumbOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    // Validate on major grid
    if (fromX % 3 != 0 || fromY % 3 != 0 || toX % 3 != 0 || toY % 3 != 0) {
      LOG_BOARD("moveDumb REJECT: not on major grid (%d,%d)->(%d,%d)", fromX, fromY, toX, toY);
      return false;
    }

    // Must be orthogonal
    if (fromX != toX && fromY != toY) {
      LOG_BOARD("moveDumb REJECT: not orthogonal (%d,%d)->(%d,%d)", fromX, fromY, toX, toY);
      return false;
    }

    // Must have a piece at source
    uint8_t piece = getPiece(fromX, fromY);
    if (piece == PIECE_NONE) {
      LOG_BOARD("moveDumb REJECT: no piece at (%d,%d)", fromX, fromY);
      return false;
    }

    // Can't move to occupied square
    if (getPiece(toX, toY) != PIECE_NONE) {
      LOG_BOARD("moveDumb REJECT: destination (%d,%d) occupied", toX, toY);
      return false;
    }

    LOG_BOARD("moveDumb: moving piece %d from (%d,%d) to (%d,%d)", piece, fromX, fromY, toX, toY);

    // Build path — step by 3 along the axis
    int8_t dx = 0, dy = 0;
    if (toX > fromX) dx = 3;
    else if (toX < fromX) dx = -3;
    if (toY > fromY) dy = 3;
    else if (toY < fromY) dy = -3;

    int8_t cx = fromX, cy = fromY;

    // Pulse each coil along the path (skip the source, include destination)
    while (cx != toX || cy != toY) {
      cx += dx;
      cy += dy;

      int8_t bit = coordToBit(cx, cy);
      if (bit < 0) {
        LOG_BOARD("moveDumb ABORT: no coil at (%d,%d) along path", cx, cy);
        return false;
      }

      LOG_BOARD("moveDumb: pulsing (%d,%d) for %dms", cx, cy, MOVE_PULSE_MS);
      if (!hw_.pulseBit((uint8_t)bit, MOVE_PULSE_MS)) {
        LOG_BOARD("moveDumb ABORT: pulse failed at (%d,%d)", cx, cy);
        return false;
      }
    }

    // Update piece state
    pieces_[fromX][fromY] = PIECE_NONE;
    pieces_[toX][toY] = piece;
    LOG_BOARD("moveDumb OK: piece %d now at (%d,%d)", piece, toX, toY);
    return true;
  }

  // ── Coil Control ──────────────────────────────────────────

  PulseCoilResponse pulseCoil(uint8_t x, uint8_t y, uint16_t duration_ms) {
    PulseCoilResponse res = { false, PulseError::NONE };

    LOG_BOARD("pulseCoil: request at grid (%d,%d) for %dms", x, y, duration_ms);

    if (x >= GRID_COLS || y >= GRID_ROWS) {
      LOG_BOARD("pulseCoil REJECT: (%d,%d) out of bounds (grid is %dx%d)", x, y, GRID_COLS, GRID_ROWS);
      res.error = PulseError::INVALID_COIL;
      return res;
    }

    if (duration_ms > MAX_PULSE_MS) {
      LOG_BOARD("pulseCoil REJECT: %dms exceeds max %dms", duration_ms, MAX_PULSE_MS);
      res.error = PulseError::PULSE_TOO_LONG;
      return res;
    }

    int8_t bit = coordToBit(x, y);
    if (bit < 0) {
      uint8_t lx = x % SR_BLOCK, ly = y % SR_BLOCK;
      LOG_BOARD("pulseCoil REJECT: (%d,%d) has no coil (local %d,%d in SR block %d,%d)", x, y, lx, ly, x / SR_BLOCK, y / SR_BLOCK);
      res.error = PulseError::INVALID_COIL;
      return res;
    }

    uint8_t sr = bit / 8, pin = bit % 8;
    LOG_BOARD("pulseCoil: (%d,%d) -> SR%d pin %d (global bit %d), delegating to hw", x, y, sr, pin, bit);
    if (!hw_.pulseBit((uint8_t)bit, duration_ms)) {
      LOG_BOARD("pulseCoil FAIL: hw refused pulse on SR%d pin %d (thermal limit)", sr, pin);
      res.error = PulseError::THERMAL_LIMIT;
      return res;
    }

    LOG_BOARD("pulseCoil OK: (%d,%d) pulsed for %dms via SR%d pin %d", x, y, duration_ms, sr, pin);
    res.success = true;
    return res;
  }

  // ── Board State ───────────────────────────────────────────

  GetBoardStateResponse getBoardState() {
    GetBoardStateResponse res = {};

    uint16_t raw[NUM_HALL_SENSORS];
    hw_.readAllSensors(raw, NUM_HALL_SENSORS);
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      int col = i % SENSOR_COLS;
      int row = i / SENSOR_COLS;
      res.raw_strengths[col][row] = raw[i];
    }

    res.piece_count = 0;
    return res;
  }

  // ── RGB ───────────────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) {
    LOG_BOARD("setRGB: r=%d g=%d b=%d (hex #%02X%02X%02X)", r, g, b, r, g, b);
    hw_.setRGB(r, g, b);
    return { true };
  }

  // ── Buttons & Power ───────────────────────────────────────

  uint16_t readButton1() { return hw_.readButton1(); }
  uint16_t readButton2() { return hw_.readButton2(); }
  bool readDC1() { return hw_.readDC1(); }
  bool readDC2() { return hw_.readDC2(); }

  // ── System ────────────────────────────────────────────────

  void shutdown() { LOG_BOARD("shutdown: delegating to hardware for safe powerdown"); hw_.shutdown(); }

private:
  Hardware hw_;
  uint8_t pieces_[GRID_COLS][GRID_ROWS];  // 10×7 piece ID grid

  // ── Coil Grid Mapping ─────────────────────────────────────
  //
  // 12 shift registers, each with 5 coils in an L-shape:
  //
  //   (0,2)  .     .       bit 4
  //   (0,1)  .     .       bit 3
  //   (0,0) (1,0) (2,0)    bit 2, bit 1, bit 0
  //
  // SRs go bottom→top in columns, then right by 3:
  //
  //        col 0-2    col 3-5    col 6-8    col 9
  //         SR2        SR5        SR8        SR11
  // row 6   (0,6)      (3,6)      (6,6)      (9,6)
  //         SR1        SR4        SR7        SR10
  // row 3   (0,3)      (3,3)      (6,3)      (9,3)
  //         SR0        SR3        SR6        SR9
  // row 0   (0,0)      (3,0)      (6,0)      (9,0)
  //
  // sr_index = col_group * 3 + row_group (column-major, bottom to top)

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

    if (ly == 0) {
      local_bit = 2 - lx;
    } else if (lx == 0) {
      local_bit = 2 + ly;
    }

    if (local_bit < 0) return -1;

    return (int8_t)(sr_index * 8 + local_bit);
  }
};
