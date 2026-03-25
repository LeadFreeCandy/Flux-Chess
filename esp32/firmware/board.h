#pragma once
#include "api.h"
#include "hardware.h"

// Piece IDs
#define PIECE_NONE  0
#define PIECE_WHITE 1
#define PIECE_BLACK 2

// Move parameters
#define MOVE_PULSE_MS       250
#define MOVE_DELAY_MS       10
#define MOVE_PULSE_REPEATS  1

FLUX_ENUM(MoveError, NONE, OUT_OF_BOUNDS, SAME_POSITION, NOT_ORTHOGONAL, NO_PIECE_AT_SOURCE, PATH_BLOCKED, COIL_FAILURE)

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

  MoveError moveDumbOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    LOG_BOARD("moveDumbOrthogonal: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    // Bounds
    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS) {
      return MoveError::OUT_OF_BOUNDS;
    }

    // Same position
    if (fromX == toX && fromY == toY) {
      return MoveError::SAME_POSITION;
    }

    // Must be orthogonal (one axis must match)
    if (fromX != toX && fromY != toY) {
      return MoveError::NOT_ORTHOGONAL;
    }

    // Must have a piece at source
    if (getPiece(fromX, fromY) == PIECE_NONE) {
      return MoveError::NO_PIECE_AT_SOURCE;
    }

    // Check path is clear (no pieces between source and destination, exclusive)
    int8_t dx = 0, dy = 0;
    if (toX > fromX) dx = 1;
    else if (toX < fromX) dx = -1;
    if (toY > fromY) dy = 1;
    else if (toY < fromY) dy = -1;

    int8_t cx = fromX + dx, cy = fromY + dy;
    while (cx != toX || cy != toY) {
      if (getPiece(cx, cy) != PIECE_NONE) {
        LOG_BOARD("moveDumbOrthogonal REJECT: path blocked at (%d,%d)", cx, cy);
        return MoveError::PATH_BLOCKED;
      }
      cx += dx;
      cy += dy;
    }
    // Destination can be occupied (capture) — caller handles that via set_piece

    // Determine step size for coil pulsing (step by 3 on major grid, 1 otherwise)
    int8_t stepX = 0, stepY = 0;
    if (toX > fromX) stepX = (fromX % 3 == 0 && toX % 3 == 0) ? 3 : 1;
    else if (toX < fromX) stepX = (fromX % 3 == 0 && toX % 3 == 0) ? -3 : -1;
    if (toY > fromY) stepY = (fromY % 3 == 0 && toY % 3 == 0) ? 3 : 1;
    else if (toY < fromY) stepY = (fromY % 3 == 0 && toY % 3 == 0) ? -3 : -1;

    // Pulse coils along the path
    uint8_t piece = pieces_[fromX][fromY];
    cx = fromX;
    cy = fromY;

    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;

      int8_t bit = coordToBit(cx, cy);
      if (bit < 0) {
        LOG_BOARD("moveDumbOrthogonal ABORT: no coil at (%d,%d)", cx, cy);
        return MoveError::COIL_FAILURE;
      }

      for (int r = 0; r < MOVE_PULSE_REPEATS; r++) {
        if (!hw_.pulseBit((uint8_t)bit, MOVE_PULSE_MS)) {
          LOG_BOARD("moveDumbOrthogonal ABORT: pulse failed at (%d,%d)", cx, cy);
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
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      int col = i % SENSOR_COLS;
      int row = i / SENSOR_COLS;
      res.raw_strengths[col][row] = raw[i];
    }

    memcpy(res.pieces, pieces_, sizeof(pieces_));
    return res;
  }

  // ── RGB / System ──────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) { hw_.setRGB(r, g, b); return { true }; }
  void shutdown() { hw_.shutdown(); }

private:
  Hardware hw_;
  uint8_t pieces_[GRID_COLS][GRID_ROWS];

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

    LOG_BOARD("initDefaultBoard: 3 white at y=0, 3 black at y=6");
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
