#pragma once
#include <Arduino.h>

// ── Grid Constants ────────────────────────────────────────────
constexpr uint8_t GRID_COLS = 12;  // 4 SR columns × 3 positions
constexpr uint8_t GRID_ROWS = 9;   // 3 SR rows × 3 positions
constexpr uint8_t MAX_PIECES = 32;
constexpr uint16_t MAX_PULSE_MS = 1000;

// ── Enum macro (define values once, get toJson for free) ─────

#define FLUX_ENUM(Name, ...)                                          \
  enum class Name : uint8_t { __VA_ARGS__ };                          \
  inline const char* toJson(Name e) {                                 \
    static const char* _names[] = { _FLUX_ENUM_STRINGS(__VA_ARGS__) };\
    uint8_t i = (uint8_t)e;                                           \
    if (i >= sizeof(_names)/sizeof(_names[0])) return "\"UNKNOWN\"";  \
    return _names[i];                                                 \
  }

// Helper: stringify each comma-separated token with quotes
#define _FLUX_STR(x) "\"" #x "\""
#define _FLUX_ENUM_STRINGS(...) _FLUX_APPLY(_FLUX_STR, __VA_ARGS__)

// Variadic apply (supports up to 16 values)
#define _FLUX_APPLY(m, ...) _FLUX_EXPAND(_FLUX_APPLY_N(__VA_ARGS__, \
  16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)(m, __VA_ARGS__))
#define _FLUX_EXPAND(x) x
#define _FLUX_APPLY_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) _FLUX_APPLY_##N
#define _FLUX_APPLY_1(m,a) m(a)
#define _FLUX_APPLY_2(m,a,...) m(a), _FLUX_APPLY_1(m,__VA_ARGS__)
#define _FLUX_APPLY_3(m,a,...) m(a), _FLUX_APPLY_2(m,__VA_ARGS__)
#define _FLUX_APPLY_4(m,a,...) m(a), _FLUX_APPLY_3(m,__VA_ARGS__)
#define _FLUX_APPLY_5(m,a,...) m(a), _FLUX_APPLY_4(m,__VA_ARGS__)
#define _FLUX_APPLY_6(m,a,...) m(a), _FLUX_APPLY_5(m,__VA_ARGS__)
#define _FLUX_APPLY_7(m,a,...) m(a), _FLUX_APPLY_6(m,__VA_ARGS__)
#define _FLUX_APPLY_8(m,a,...) m(a), _FLUX_APPLY_7(m,__VA_ARGS__)
#define _FLUX_APPLY_9(m,a,...) m(a), _FLUX_APPLY_8(m,__VA_ARGS__)
#define _FLUX_APPLY_10(m,a,...) m(a), _FLUX_APPLY_9(m,__VA_ARGS__)
#define _FLUX_APPLY_11(m,a,...) m(a), _FLUX_APPLY_10(m,__VA_ARGS__)
#define _FLUX_APPLY_12(m,a,...) m(a), _FLUX_APPLY_11(m,__VA_ARGS__)
#define _FLUX_APPLY_13(m,a,...) m(a), _FLUX_APPLY_12(m,__VA_ARGS__)
#define _FLUX_APPLY_14(m,a,...) m(a), _FLUX_APPLY_13(m,__VA_ARGS__)
#define _FLUX_APPLY_15(m,a,...) m(a), _FLUX_APPLY_14(m,__VA_ARGS__)
#define _FLUX_APPLY_16(m,a,...) m(a), _FLUX_APPLY_15(m,__VA_ARGS__)

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
