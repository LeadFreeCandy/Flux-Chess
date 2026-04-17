// Demo mode firmware — moves two pieces anti-polar around the board.
// Uses the same Board/physics code as the main firmware, no duplication.
// All moves are equal distance so moveMulti uses a single physics sim.

#include "board.h"

Board board;

// Animation: rectangular path around the board perimeter.
// Piece A follows this path forward, piece B follows it offset by half
// (anti-polar — always on the opposite side of the rectangle).
struct Waypoint { uint8_t x, y; };

static const Waypoint path[] = {
  {0, 0},  // 0: bottom-left
  {3, 0},  // 1: bottom-mid
  {6, 0},  // 2: bottom-right
  {6, 3},  // 3: right-mid
  {6, 6},  // 4: top-right
  {3, 6},  // 5: top-mid
  {0, 6},  // 6: top-left
  {0, 3},  // 7: left-mid
};

static const int PATH_LEN = sizeof(path) / sizeof(path[0]);
static const int HALF = PATH_LEN / 2;  // offset for anti-polar piece

static int indexA = 0;

// Pause between moves (ms)
static const int MOVE_DELAY = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("{\"type\":\"demo\",\"status\":\"starting\"}");

  // Override physics params with known-good demo values (skip NVS)
  PhysicsParams p;
  p.piece_mass_g         = 2.7f;
  p.max_current_a        = 0.8f;
  p.mu_static            = 0.55f;
  p.mu_kinetic           = 0.45f;
  p.target_velocity_mm_s = 150.0f;
  p.target_accel_mm_s2   = 1500.0f;
  p.max_jerk_mm_s3       = 20000.0f;
  p.coast_friction_offset = 0.0f;
  p.brake_pulse_ms       = 100;
  p.pwm_freq_hz          = 20000;
  p.pwm_compensation     = 0.1f;
  p.all_coils_equal      = false;
  p.force_scale          = 1.0f;
  p.max_duration_ms      = 5000;
  p.max_retry_attempts   = 0;
  p.tick_ms              = 10;
  board.setPhysicsParamsNoSave(p);

  // Clear board
  for (int x = 0; x < GRID_COLS; x++)
    for (int y = 0; y < GRID_ROWS; y++)
      board.setPiece(x, y, PIECE_NONE);

  // Place two pieces at anti-polar positions
  indexA = 0;
  int indexB = HALF;
  board.setPiece(path[indexA].x, path[indexA].y, PIECE_WHITE);
  board.setPiece(path[indexB].x, path[indexB].y, PIECE_BLACK);

  Serial.println("{\"type\":\"demo\",\"status\":\"ready\",\"pieces\":2}");
}

void loop() {
  int nextA = (indexA + 1) % PATH_LEN;
  int indexB = (indexA + HALF) % PATH_LEN;
  int nextB = (nextA + HALF) % PATH_LEN;

  Serial.printf("{\"type\":\"demo\",\"moveA\":{\"from\":[%d,%d],\"to\":[%d,%d]},\"moveB\":{\"from\":[%d,%d],\"to\":[%d,%d]}}\n",
    path[indexA].x, path[indexA].y, path[nextA].x, path[nextA].y,
    path[indexB].x, path[indexB].y, path[nextB].x, path[nextB].y);

  Board::MultiMoveRequest moves[2] = {
    { path[indexA].x, path[indexA].y, path[nextA].x, path[nextA].y },
    { path[indexB].x, path[indexB].y, path[nextB].x, path[nextB].y },
  };

  String result = board.moveMulti(moves, 2);
  Serial.println(result);

  if (result.indexOf("\"success\":false") >= 0) {
    Serial.println("{\"type\":\"demo\",\"error\":\"moveMulti failed\"}");
    while (true) delay(100);
  }

  indexA = nextA;
  delay(MOVE_DELAY);
}
