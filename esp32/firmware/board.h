#pragma once
#include "api.h"
#include "hardware.h"

class Board {
public:
  Board() {}

  // ── Coil Control ──────────────────────────────────────────

  PulseCoilResponse pulseCoil(uint8_t x, uint8_t y, uint16_t duration_ms) {
    PulseCoilResponse res = { false, PulseError::NONE };

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

    // 12 hall sensors map to a 4×3 grid
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
    hw_.setRGB(r, g, b);
    return { true };
  }

  // ── Buttons & Power ───────────────────────────────────────

  uint16_t readButton1() { return hw_.readButton1(); }
  uint16_t readButton2() { return hw_.readButton2(); }
  bool readDC1() { return hw_.readDC1(); }
  bool readDC2() { return hw_.readDC2(); }

  // ── System ────────────────────────────────────────────────

  void shutdown() { hw_.shutdown(); }

private:
  Hardware hw_;

  // ── Coil Grid Mapping ─────────────────────────────────────
  //
  // 12 shift registers in a 4-column × 3-row grid.
  // Each SR drives 5 coils in an L-shape within a 3×3 block:
  //
  //   (0,2)  .     .       bit 4
  //   (0,1)  .     .       bit 3
  //   (0,0) (1,0) (2,0)    bit 2, bit 1, bit 0
  //
  // SR layout (4 cols × 3 rows, each offset by 3):
  //   SR0  @ (0,0)   SR1  @ (3,0)   SR2  @ (6,0)   SR3  @ (9,0)
  //   SR4  @ (0,3)   SR5  @ (3,3)   SR6  @ (6,3)   SR7  @ (9,3)
  //   SR8  @ (0,6)   SR9  @ (3,6)   SR10 @ (6,6)   SR11 @ (9,6)

  static constexpr uint8_t SR_COLS = 4;
  static constexpr uint8_t SR_ROWS = 3;
  static constexpr uint8_t SR_BLOCK = 3;  // 3×3 block per SR

  // Returns global bit index for (x, y), or -1 if no coil at that position
  static int8_t coordToBit(uint8_t x, uint8_t y) {
    // Which SR block?
    uint8_t sr_col = x / SR_BLOCK;
    uint8_t sr_row = y / SR_BLOCK;
    if (sr_col >= SR_COLS || sr_row >= SR_ROWS) return -1;

    uint8_t sr_index = sr_row * SR_COLS + sr_col;

    // Local position within the 3×3 block
    uint8_t lx = x % SR_BLOCK;
    uint8_t ly = y % SR_BLOCK;

    int8_t local_bit = -1;

    if (ly == 0) {
      // Bottom row: bit 2 at lx=0, bit 1 at lx=1, bit 0 at lx=2
      local_bit = 2 - lx;
    } else if (lx == 0) {
      // Left column going up: bit 3 at ly=1, bit 4 at ly=2
      local_bit = 2 + ly;
    }
    // else: no coil at this position (lx>0 && ly>0)

    if (local_bit < 0) return -1;

    return (int8_t)(sr_index * 8 + local_bit);
  }
};
