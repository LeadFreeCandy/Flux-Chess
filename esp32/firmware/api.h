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

struct PlayNoteRequest {
  uint8_t x;
  uint8_t y;
  uint16_t freq_hz;
  uint16_t duration_ms;
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

// Piece constants
constexpr uint8_t PIECE_NONE = 0;
constexpr uint8_t PIECE_WHITE = 1;
constexpr uint8_t PIECE_BLACK = 2;

struct SetPieceRequest {
  uint8_t x;
  uint8_t y;
  uint8_t id;
};

struct CalibrateRequest {};

struct CalibrateResponse {
  bool success;
  String toJson() const { return Json().add("success", success).build(); }
};

struct GetCalibrationRequest {};

struct GetCalibrationResponse {
  String data;
  String toJson() const { return data; }
};

FLUX_ENUM(MoveError, NONE, OUT_OF_BOUNDS, SAME_POSITION, NOT_ORTHOGONAL, NO_PIECE_AT_SOURCE, PATH_BLOCKED, COIL_FAILURE)

struct MoveDumbRequest {
  uint8_t from_x;
  uint8_t from_y;
  uint8_t to_x;
  uint8_t to_y;
};

static constexpr int MAX_DIAG_COILS = 9;  // longest possible orthogonal path

struct CoilDiag {
  uint8_t sensor_idx = 0;
  uint16_t min_reading = 0xFFFF;  // lowest ADC value seen (lower = stronger detection)
  bool detected = false;          // did min_reading cross threshold?
  uint16_t arrival_reading = 0;   // reading after move completes
};

struct MoveDiag {
  CoilDiag coils[MAX_DIAG_COILS];
  uint8_t num_coils = 0;
  bool checkpoint_ok = false;
  uint8_t retries_used = 0;

  String toJson() const {
    String arr = "[";
    for (int i = 0; i < num_coils; i++) {
      if (i > 0) arr += ",";
      arr += "{\"sensor\":";  arr += String(coils[i].sensor_idx);
      arr += ",\"min\":";     arr += String(coils[i].min_reading);
      arr += ",\"detected\":"; arr += coils[i].detected ? "true" : "false";
      arr += ",\"arrival\":"; arr += String(coils[i].arrival_reading);
      arr += "}";
    }
    arr += "]";
    return Json().add("checkpoint_ok", checkpoint_ok)
                 .add("retries_used", retries_used)
                 .addRaw("coils", arr)
                 .build();
  }
};

struct MoveResponse {
  bool success;
  MoveError error;
  bool has_diag = false;
  MoveDiag diag;

  String toJson() const {
    Json j;
    j.add("success", success).add("error", error);
    if (has_diag) j.addRaw("diag", diag.toJson());
    return j.build();
  }
};

struct MovePhysicsRequest {
  uint8_t from_x;
  uint8_t from_y;
  uint8_t to_x;
  uint8_t to_y;
};

struct SetPhysicsParamsRequest {
  float piece_mass_g;
  float max_current_a;
  float mu_static;
  float mu_kinetic;
  float target_velocity_mm_s;
  float target_accel_mm_s2;
  float max_jerk_mm_s3;
  float coast_friction_offset;
  uint16_t brake_pulse_ms;
  uint16_t pwm_freq_hz;
  float pwm_compensation;
  bool all_coils_equal;
  float force_scale;
  uint16_t max_duration_ms;
  uint8_t max_retry_attempts;
  uint8_t tick_ms;
};

struct DiagonalTestRequest {
  uint8_t from_x;
  uint8_t from_y;
  uint8_t to_x;
  uint8_t to_y;
  uint16_t catapult_ms;
  uint8_t catapult_duty;
  uint16_t delay1_ms;
  uint16_t catch_ms;
  uint8_t catch_duty;
  uint16_t delay2_ms;
  uint16_t center_ms;
};

struct DiagonalTestResponse {
  bool success;
  String toJson() const { return Json().add("success", success).build(); }
};

struct MoveMultiRequest {
  uint8_t count;
};

// ── Command Table ─────────────────────────────────────────────
// Parsed by codegen/generate.py — do not change format
// API_COMMAND(shutdown, POST, /api/shutdown, ShutdownRequest, ShutdownResponse)
// API_COMMAND(pulse_coil, POST, /api/pulse_coil, PulseCoilRequest, PulseCoilResponse)
// API_COMMAND(get_board_state, GET, /api/board_state, GetBoardStateRequest, GetBoardStateResponse)
// API_COMMAND(set_rgb, POST, /api/set_rgb, SetRGBRequest, SetRGBResponse)
// API_COMMAND(set_piece, POST, /api/set_piece, SetPieceRequest, SetRGBResponse)
// API_COMMAND(move_dumb, POST, /api/move_dumb, MoveDumbRequest, MoveResponse)
// API_COMMAND(move_physics, POST, /api/move_physics, MovePhysicsRequest, MoveResponse)
// API_COMMAND(move_piece, POST, /api/move_piece, MoveDumbRequest, MoveResponse)
// API_COMMAND(hexapawn_play, POST, /api/hexapawn/play, CalibrateRequest, GetCalibrationResponse)
// API_COMMAND(set_physics_params, POST, /api/set_physics_params, SetPhysicsParamsRequest, ShutdownResponse)
// API_COMMAND(get_physics_params, GET, /api/get_physics_params, GetBoardStateRequest, GetCalibrationResponse)
// API_COMMAND(tune_physics, POST, /api/tune_physics, CalibrateRequest, GetCalibrationResponse)
// API_COMMAND(calibrate, POST, /api/calibrate, CalibrateRequest, CalibrateResponse)
// API_COMMAND(get_calibration, GET, /api/calibration, GetCalibrationRequest, GetCalibrationResponse)
// API_COMMAND(diagonal_test, POST, /api/diagonal_test, DiagonalTestRequest, DiagonalTestResponse)
// API_COMMAND(move_multi, POST, /api/move_multi, MoveMultiRequest, MoveResponse)
// API_COMMAND(edge_move_test, POST, /api/edge_move_test, MoveMultiRequest, DiagonalTestResponse)
// API_COMMAND(play_note, POST, /api/play_note, PlayNoteRequest, PulseCoilResponse)
