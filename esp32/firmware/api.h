#pragma once
#include <Arduino.h>
#include "macros.h"

// ── Grid Constants ────────────────────────────────────────────
constexpr uint8_t GRID_COLS = 10;  // 3 full SR columns + 1 partial
constexpr uint8_t GRID_ROWS = 7;   // 2 full SR rows + 1 partial
constexpr uint8_t MAX_PIECES = 32;
constexpr uint16_t MAX_PULSE_MS = 1000;

// ── Enums ────────────────────────────────────────────────────

FLUX_ENUM(PulseError, NONE, INVALID_COIL, PULSE_TOO_LONG, THERMAL_LIMIT)

// ── JSON Builder Helper ──────────────────────────────────────

class Json {
public:
  Json() { buf_ = "{"; }

  Json& add(const char* key, bool val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":";
    buf_ += val ? "true" : "false";
    return *this;
  }

  Json& add(const char* key, int val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":";
    buf_ += String(val);
    return *this;
  }

  Json& add(const char* key, uint16_t val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":";
    buf_ += String(val);
    return *this;
  }

  Json& add(const char* key, const char* val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":";
    buf_ += val;  // Already quoted for enums, or raw JSON
    return *this;
  }

  Json& addStr(const char* key, const char* val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":\"";
    buf_ += val; buf_ += "\"";
    return *this;
  }

  Json& addRaw(const char* key, const String& val) {
    sep(); buf_ += "\""; buf_ += key; buf_ += "\":";
    buf_ += val;
    return *this;
  }

  String build() { return buf_ + "}"; }

private:
  String buf_;
  bool first_ = true;
  void sep() { if (!first_) buf_ += ","; first_ = false; }
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

// ── Request / Response Structs (with toJson) ──────────────────

struct ShutdownRequest {};
struct ShutdownResponse {
  String toJson() const { return "{}"; }
};

struct PulseCoilRequest {
  uint8_t x;
  uint8_t y;
  uint16_t duration_ms;
};

struct PulseCoilResponse {
  bool success;
  PulseError error;

  String toJson() const {
    return Json().add("success", success).add("error", ::toJson(error)).build();
  }
};

struct GetBoardStateRequest {};

struct GetBoardStateResponse {
  uint16_t raw_strengths[GRID_COLS][GRID_ROWS];
  PiecePosition pieces[MAX_PIECES];
  uint8_t piece_count;

  String toJson() const {
    String json = "{\"raw_strengths\":[";
    for (int x = 0; x < GRID_COLS; x++) {
      json += "[";
      for (int y = 0; y < GRID_ROWS; y++) {
        json += String(raw_strengths[x][y]);
        if (y < GRID_ROWS - 1) json += ",";
      }
      json += "]";
      if (x < GRID_COLS - 1) json += ",";
    }
    json += "],\"piece_count\":" + String(piece_count) + "}";
    return json;
  }
};

struct SetRGBRequest {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct SetRGBResponse {
  bool success;

  String toJson() const {
    return Json().add("success", success).build();
  }
};

// ── Command Table ─────────────────────────────────────────────
// Parsed by codegen/generate.py — do not change format
// API_COMMAND(shutdown, POST, /api/shutdown, ShutdownRequest, ShutdownResponse)
// API_COMMAND(pulse_coil, POST, /api/pulse_coil, PulseCoilRequest, PulseCoilResponse)
// API_COMMAND(get_board_state, GET, /api/board_state, GetBoardStateRequest, GetBoardStateResponse)
// API_COMMAND(set_rgb, POST, /api/set_rgb, SetRGBRequest, SetRGBResponse)
