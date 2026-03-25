#pragma once
#include "api.h"
#include "hardware.h"

// Piece IDs
#define PIECE_NONE  0
#define PIECE_WHITE 1
#define PIECE_BLACK 2

// Move parameters
#define MOVE_PULSE_MS       250   // ms per coil pulse during move
#define MOVE_DELAY_MS       10    // ms delay between sequential pulses
#define MOVE_PULSE_REPEATS  1     // times to repeat each pulse

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
    LOG_BOARD("setPiece: (%d,%d) = %d", x, y, id);
  }

  // ── Move (public API) ──────────────────────────────────────
  // Path-plans an L-shaped or straight move between any two
  // major grid positions. Breaks into orthogonal segments.

  MoveError moveDumb(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    LOG_BOARD("moveDumb: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    // Validate both positions on major grid
    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS) {
      LOG_BOARD("moveDumb REJECT: out of bounds");
      return MoveError::OUT_OF_BOUNDS;
    }
    if (fromX % 3 != 0 || fromY % 3 != 0 || toX % 3 != 0 || toY % 3 != 0) {
      LOG_BOARD("moveDumb REJECT: not on major grid");
      return MoveError::NOT_ON_MAJOR_GRID;
    }
    if (fromX == toX && fromY == toY) {
      LOG_BOARD("moveDumb REJECT: same position");
      return MoveError::SAME_POSITION;
    }
    if (getPiece(fromX, fromY) == PIECE_NONE) {
      LOG_BOARD("moveDumb REJECT: no piece at source");
      return MoveError::NO_PIECE_AT_SOURCE;
    }
    if (getPiece(toX, toY) != PIECE_NONE) {
      LOG_BOARD("moveDumb REJECT: destination occupied");
      return MoveError::DESTINATION_OCCUPIED;
    }

    // Path planning: move X first, then Y (simple L-path)
    MoveError err;

    if (fromX != toX) {
      err = moveDumbOrthogonal(fromX, fromY, toX, fromY);
      if (err != MoveError::NONE) return err;
    }

    if (fromY != toY) {
      err = moveDumbOrthogonal(toX, fromY, toX, toY);
      if (err != MoveError::NONE) return err;
    }

    LOG_BOARD("moveDumb OK: piece now at (%d,%d)", toX, toY);
    return MoveError::NONE;
  }

  // ── Coil Control ──────────────────────────────────────────

  PulseCoilResponse pulseCoil(uint8_t x, uint8_t y, uint16_t duration_ms) {
    PulseCoilResponse res = { false, PulseError::NONE };

    LOG_BOARD("pulseCoil: request at grid (%d,%d) for %dms", x, y, duration_ms);

    if (x >= GRID_COLS || y >= GRID_ROWS) {
      res.error = PulseError::INVALID_COIL;
      return res;
    }
    if (duration_ms > MAX_PULSE_MS) {
      res.error = PulseError::PULSE_TOO_LONG;
      return res;
    }

    int8_t bit = coordToBit(x, y);
    if (bit < 0) {
      res.error = PulseError::INVALID_COIL;
      return res;
    }

    if (!hw_.pulseBit((uint8_t)bit, duration_ms)) {
      res.error = PulseError::THERMAL_LIMIT;
      return res;
    }

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

  // ── RGB ───────────────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) {
    hw_.setRGB(r, g, b);
    return { true };
  }

  // ── System ────────────────────────────────────────────────

  void shutdown() { hw_.shutdown(); }

private:
  Hardware hw_;
  uint8_t pieces_[GRID_COLS][GRID_ROWS];

  // ── Default Board Setup ─────────────────────────────────────
  // Places pieces on the standard chess starting positions
  // mapped to the major grid (every 3rd position).
  // Major grid positions: x = 0,3,6,9  y = 0,3,6
  // That gives us a 4x3 grid of major squares.
  //
  //   y=6:  B B B B    (black back row — but only 4 columns)
  //   y=3:  . . . .    (empty middle)
  //   y=0:  W W W W    (white back row)

  void initDefaultBoard() {
    memset(pieces_, PIECE_NONE, sizeof(pieces_));

    // White pieces on bottom row (y=0)
    for (uint8_t x = 0; x < GRID_COLS; x += 3) {
      pieces_[x][0] = PIECE_WHITE;
    }
    // Black pieces on top row (y=6)
    for (uint8_t x = 0; x < GRID_COLS; x += 3) {
      pieces_[x][6] = PIECE_BLACK;
    }

    LOG_BOARD("initDefaultBoard: 4 white at y=0, 4 black at y=6");
  }

  // ── Orthogonal Move (internal) ──────────────────────────────
  // Moves a piece in a straight line along one axis.
  // Updates piece state. Called by moveDumb for each leg.

  MoveError moveDumbOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    // Must be orthogonal
    if (fromX != toX && fromY != toY) {
      return MoveError::NOT_ORTHOGONAL;
    }
    if (fromX == toX && fromY == toY) {
      return MoveError::NONE;  // no-op
    }

    uint8_t piece = getPiece(fromX, fromY);

    // Step direction
    int8_t dx = 0, dy = 0;
    if (toX > fromX) dx = 3;
    else if (toX < fromX) dx = -3;
    if (toY > fromY) dy = 3;
    else if (toY < fromY) dy = -3;

    int8_t cx = fromX, cy = fromY;

    LOG_BOARD("moveOrthogonal: (%d,%d)->(%d,%d) step=(%d,%d)", fromX, fromY, toX, toY, dx, dy);

    while (cx != toX || cy != toY) {
      cx += dx;
      cy += dy;

      int8_t bit = coordToBit(cx, cy);
      if (bit < 0) {
        LOG_BOARD("moveOrthogonal ABORT: no coil at (%d,%d)", cx, cy);
        return MoveError::COIL_FAILURE;
      }

      // Pulse with repeats
      for (int r = 0; r < MOVE_PULSE_REPEATS; r++) {
        LOG_BOARD("moveOrthogonal: pulse (%d,%d) rep %d/%d for %dms", cx, cy, r + 1, MOVE_PULSE_REPEATS, MOVE_PULSE_MS);
        if (!hw_.pulseBit((uint8_t)bit, MOVE_PULSE_MS)) {
          LOG_BOARD("moveOrthogonal ABORT: pulse failed at (%d,%d)", cx, cy);
          return MoveError::COIL_FAILURE;
        }
        if (r < MOVE_PULSE_REPEATS - 1) {
          delay(MOVE_DELAY_MS);
        }
      }

      // Delay between steps
      delay(MOVE_DELAY_MS);
    }

    // Update piece state
    pieces_[fromX][fromY] = PIECE_NONE;
    pieces_[toX][toY] = piece;
    return MoveError::NONE;
  }

  // ── Coil Grid Mapping ─────────────────────────────────────
  //
  // SRs go bottom→top in columns, then right by 3:
  //        col 0-2    col 3-5    col 6-8    col 9
  //         SR2        SR5        SR8        SR11
  // row 6   (0,6)      (3,6)      (6,6)      (9,6)
  //         SR1        SR4        SR7        SR10
  // row 3   (0,3)      (3,3)      (6,3)      (9,3)
  //         SR0        SR3        SR6        SR9
  // row 0   (0,0)      (3,0)      (6,0)      (9,0)
  //
  // sr_index = col_group * 3 + row_group

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
