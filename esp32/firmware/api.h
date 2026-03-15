#pragma once
#include <Arduino.h>

// ── Grid Constants ────────────────────────────────────────────
constexpr uint8_t GRID_COLS = 10;
constexpr uint8_t GRID_ROWS = 7;
constexpr uint8_t MAX_PIECES = 32;
constexpr uint16_t MAX_PULSE_MS = 1000;

// ── Enums ─────────────────────────────────────────────────────

enum class PulseError : uint8_t {
  NONE,
  INVALID_COIL,
  PULSE_TOO_LONG,
  THERMAL_LIMIT
};

// ── Shared Types ──────────────────────────────────────────────

struct Position {
  uint8_t x;
  uint8_t y;
};

struct PiecePosition {
  uint8_t piece_id;
  Position pos;
};

// ── Request / Response Structs ────────────────────────────────

struct ShutdownRequest {};
struct ShutdownResponse {};

struct PulseCoilRequest {
  uint8_t x;
  uint8_t y;
  uint16_t duration_ms;
};

struct PulseCoilResponse {
  bool success;
  PulseError error;
};

struct GetBoardStateRequest {};

struct GetBoardStateResponse {
  uint16_t raw_strengths[GRID_COLS][GRID_ROWS];
  PiecePosition pieces[MAX_PIECES];
  uint8_t piece_count;
};

struct SetRGBRequest {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct SetRGBResponse {
  bool success;
};

// ── Command Table ─────────────────────────────────────────────
// Parsed by codegen/generate.py — do not change format
// API_COMMAND(shutdown, POST, /api/shutdown, ShutdownRequest, ShutdownResponse)
// API_COMMAND(pulse_coil, POST, /api/pulse_coil, PulseCoilRequest, PulseCoilResponse)
// API_COMMAND(get_board_state, GET, /api/board_state, GetBoardStateRequest, GetBoardStateResponse)
// API_COMMAND(set_rgb, POST, /api/set_rgb, SetRGBRequest, SetRGBResponse)
