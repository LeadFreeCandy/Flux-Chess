#pragma once
#include "utils.h"

// ── Grid Constants ────────────────────────────────────────────
constexpr uint8_t GRID_COLS = 10;  // 3 full SR columns + 1 partial
constexpr uint8_t GRID_ROWS = 7;   // 2 full SR rows + 1 partial
constexpr uint8_t SENSOR_COLS = 4; // Hall sensor grid
constexpr uint8_t SENSOR_ROWS = 3;
constexpr uint8_t MAX_PIECES = 32;
constexpr uint16_t MAX_PULSE_MS = 1000;

// ── Enums ────────────────────────────────────────────────────

FLUX_ENUM(PulseError, NONE, INVALID_COIL, PULSE_TOO_LONG, THERMAL_LIMIT)

FLUX_ENUM(MoveError, NONE, OUT_OF_BOUNDS, NOT_ON_MAJOR_GRID, NO_PIECE_AT_SOURCE, DESTINATION_OCCUPIED, NOT_ORTHOGONAL, COIL_FAILURE, SAME_POSITION)

// ── Shared Types ──────────────────────────────────────────────

struct Position {
  uint8_t x;
  uint8_t y;
};

struct PiecePosition {
  uint8_t piece_id;
  Position pos;
};

// ── Requests ──────────────────────────────────────────────────

struct ShutdownRequest {};

struct PulseCoilRequest {
  uint8_t x;
  uint8_t y;
  uint16_t duration_ms;
};

struct GetBoardStateRequest {};

struct SetRGBRequest {
  uint8_t r;
  uint8_t g;
  uint8_t b;
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
  uint16_t raw_strengths[SENSOR_COLS][SENSOR_ROWS];
  uint8_t pieces[GRID_COLS][GRID_ROWS];

  String toJson() const {
    // Sensor grid
    String sensors = "[";
    for (int x = 0; x < SENSOR_COLS; x++) {
      sensors += "[";
      for (int y = 0; y < SENSOR_ROWS; y++) {
        sensors += String(raw_strengths[x][y]);
        if (y < SENSOR_ROWS - 1) sensors += ",";
      }
      sensors += "]";
      if (x < SENSOR_COLS - 1) sensors += ",";
    }
    sensors += "]";

    // Pieces grid
    String pcs = "[";
    for (int x = 0; x < GRID_COLS; x++) {
      pcs += "[";
      for (int y = 0; y < GRID_ROWS; y++) {
        pcs += String(pieces[x][y]);
        if (y < GRID_ROWS - 1) pcs += ",";
      }
      pcs += "]";
      if (x < GRID_COLS - 1) pcs += ",";
    }
    pcs += "]";

    return Json().addRaw("raw_strengths", sensors).addRaw("pieces", pcs).build();
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
