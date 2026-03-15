#pragma once
#include "utils.h"

// ── Grid Constants ────────────────────────────────────────────
constexpr uint8_t GRID_COLS = 10;  // 3 full SR columns + 1 partial
constexpr uint8_t GRID_ROWS = 7;   // 2 full SR rows + 1 partial
constexpr uint8_t MAX_PIECES = 32;
constexpr uint16_t MAX_PULSE_MS = 1000;

// ── Enums ────────────────────────────────────────────────────

FLUX_ENUM(PulseError, NONE, INVALID_COIL, PULSE_TOO_LONG, THERMAL_LIMIT)

// ── Shared Types ──────────────────────────────────────────────

struct Position {
  uint8_t x;
  uint8_t y;
};

struct PiecePosition {
  uint8_t piece_id;
  Position pos;
};

// ── Responses ─────────────────────────────────────────────────

struct ShutdownResponse {
  String toJson() const { return "{}"; }
};

struct PulseCoilResponse {
  bool success;
  PulseError error;
  String toJson() const { return Json().add("success", success).add("error", error).build(); }
};

struct GetBoardStateResponse {
  uint16_t raw_strengths[GRID_COLS][GRID_ROWS];
  PiecePosition pieces[MAX_PIECES];
  uint8_t piece_count;

  String toJson() const {
    String arr = "[";
    for (int x = 0; x < GRID_COLS; x++) {
      arr += "[";
      for (int y = 0; y < GRID_ROWS; y++) {
        arr += String(raw_strengths[x][y]);
        if (y < GRID_ROWS - 1) arr += ",";
      }
      arr += "]";
      if (x < GRID_COLS - 1) arr += ",";
    }
    arr += "]";
    return Json().addRaw("raw_strengths", arr).add("piece_count", piece_count).build();
  }
};

struct SetRGBResponse {
  bool success;
  String toJson() const { return Json().add("success", success).build(); }
};

// ── Command Table ─────────────────────────────────────────────
// Parsed by codegen/generate.py — do not change format
// API_COMMAND(shutdown, POST, /api/shutdown, ShutdownRequest, ShutdownResponse)
// API_COMMAND(pulse_coil, POST, /api/pulse_coil, PulseCoilRequest, PulseCoilResponse)
// API_COMMAND(get_board_state, GET, /api/board_state, GetBoardStateRequest, GetBoardStateResponse)
// API_COMMAND(set_rgb, POST, /api/set_rgb, SetRGBRequest, SetRGBResponse)
