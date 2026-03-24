#pragma once
#include "api.h"
#include "hardware.h"


class Board {
public:
  Board() {}

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
  // row 5   (0,5)      (3,5)      (6,5)      (9,5)
  // row 4   (0,4)      (3,4)      (6,4)      (9,4)
  //         SR1        SR4        SR7        SR10
  // row 3   (0,3)      (3,3)      (6,3)      (9,3)
  // row 2   (0,2)      (3,2)      (6,2)      (9,2)
  // row 1   (0,1)      (3,1)      (6,1)      (9,1)
  //         SR0        SR3        SR6        SR9
  // row 0   (0,0)      (3,0)      (6,0)      (9,0)
  //
  // SR chain: SR0→SR1→SR2→SR3→SR4→...→SR11
  // sr_index = col_group * 3 + row_group (column-major, bottom to top)

  static constexpr uint8_t SR_COLS = 4;   // 4 column groups
  static constexpr uint8_t SR_ROWS = 3;   // 3 row groups per column
  static constexpr uint8_t SR_BLOCK = 3;  // 3×3 block per SR

  static int8_t coordToBit(uint8_t x, uint8_t y) {
    uint8_t sr_col = x / SR_BLOCK;
    uint8_t sr_row = y / SR_BLOCK;
    if (sr_col >= SR_COLS || sr_row >= SR_ROWS) return -1;

    // Column-major: go up first, then right
    uint8_t sr_index = sr_col * SR_ROWS + sr_row;

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
