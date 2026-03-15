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

    // TODO: populate grid mapping table
    // uint8_t coilBit = grid_to_coil_[x][y];
    // if (!hw_.pulseCoil(coilBit, duration_ms)) {
    //   res.error = PulseError::THERMAL_LIMIT;
    //   return res;
    // }

    res.success = true;
    return res;
  }

  // ── Board State ───────────────────────────────────────────

  GetBoardStateResponse getBoardState() {
    GetBoardStateResponse res = {};

    // Read raw sensor values into the grid
    // For now, sensors map linearly: sensor i → col i%GRID_COLS, row i/GRID_COLS
    uint16_t raw[NUM_HALL_SENSORS];
    hw_.readAllSensors(raw, NUM_HALL_SENSORS);
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      int col = i % GRID_COLS;
      int row = i / GRID_COLS;
      res.raw_strengths[col][row] = raw[i];
    }

    // TODO: Piece detection from sensor data
    res.piece_count = 0;

    return res;
  }

  // ── RGB ───────────────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) {
    // TODO: Drive RGB LEDs when hardware is connected
    (void)r; (void)g; (void)b;
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
};
