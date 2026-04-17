// Demo mode — auto-detects pieces and moves them randomly.
// Each loop: scan all square-grid positions (0,3,6 x 0,3,6) for pieces
// via hall sensors. All detected pieces move 1 square (3 coils) in a
// random orthogonal direction. No pieces = idle. Add/remove pieces
// on the fly and the demo adapts.

#include "board.h"

Board board;

struct Pos { uint8_t x, y; };

static const int MOVE_DELAY = 150;
static const uint8_t STEP = 3;
static const uint8_t MAX_X = 6;
static const uint8_t MAX_Y = 6;
static const int DEMO_MAX_PIECES = 4;  // moveMulti limit

// Track last direction per grid position to avoid reversals
static int8_t lastDirAt[3][3];  // indexed by x/3, y/3

// Directions: 0=right, 1=left, 2=up, 3=down
static const int8_t dirs[4][2] = {{3,0},{-3,0},{0,3},{0,-3}};

int8_t opposite(int8_t d) {
  if (d < 0) return -1;
  return d ^ 1;
}

bool inBounds(int x, int y) {
  return x >= 0 && x <= MAX_X && y >= 0 && y <= MAX_Y;
}

bool posInList(int x, int y, const Pos* list, int n) {
  for (int i = 0; i < n; i++) {
    if (list[i].x == x && list[i].y == y) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("{\"type\":\"demo_random\",\"status\":\"starting\"}");

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

  // Clear board state
  for (int x = 0; x < GRID_COLS; x++)
    for (int y = 0; y < GRID_ROWS; y++)
      board.setPiece(x, y, PIECE_NONE);

  memset(lastDirAt, -1, sizeof(lastDirAt));
  randomSeed(analogRead(0));

  Serial.println("{\"type\":\"demo_random\",\"status\":\"ready\"}");
}

void loop() {
  // ── Scan for pieces ──
  Pos detected[DEMO_MAX_PIECES];
  int nDetected = 0;

  for (uint8_t gx = 0; gx <= MAX_X && nDetected < DEMO_MAX_PIECES; gx += STEP) {
    for (uint8_t gy = 0; gy <= MAX_Y && nDetected < DEMO_MAX_PIECES; gy += STEP) {
      if (board.detectPiece(gx, gy)) {
        detected[nDetected++] = {gx, gy};
        // Ensure board state matches sensor reality
        if (board.getPiece(gx, gy) == PIECE_NONE) {
          board.setPiece(gx, gy, PIECE_WHITE);
        }
      } else {
        // Piece removed — clear board state
        if (board.getPiece(gx, gy) != PIECE_NONE) {
          board.setPiece(gx, gy, PIECE_NONE);
          lastDirAt[gx / STEP][gy / STEP] = -1;
        }
      }
    }
  }

  if (nDetected == 0) {
    delay(MOVE_DELAY);
    return;
  }

  // ── Assign random destinations ──
  Pos dest[DEMO_MAX_PIECES];
  int8_t chosenDir[DEMO_MAX_PIECES];
  Pos occupied[DEMO_MAX_PIECES];  // destinations already claimed
  int nOccupied = 0;

  // Also treat positions of pieces NOT moving as occupied
  // (all detected pieces will try to move, so initially no extras)

  for (int i = 0; i < nDetected; i++) {
    Pos p = detected[i];
    int8_t prevDir = lastDirAt[p.x / STEP][p.y / STEP];
    int8_t rev = opposite(prevDir);

    // Collect valid moves (non-reverse, in-bounds, no collision)
    int8_t candDirs[4]; Pos candPos[4]; int nCand = 0;

    for (int d = 0; d < 4; d++) {
      if (d == rev) continue;
      int nx = p.x + dirs[d][0];
      int ny = p.y + dirs[d][1];
      if (!inBounds(nx, ny)) continue;
      // Can't go where another detected piece currently is (unless it's also moving away)
      // Simplification: block destinations already claimed + positions of later pieces
      if (posInList(nx, ny, occupied, nOccupied)) continue;
      // Block positions of pieces not yet assigned (they're still there)
      bool blocked = false;
      for (int j = i + 1; j < nDetected; j++) {
        if (detected[j].x == nx && detected[j].y == ny) { blocked = true; break; }
      }
      if (blocked) continue;
      candDirs[nCand] = d;
      candPos[nCand] = {(uint8_t)nx, (uint8_t)ny};
      nCand++;
    }

    // Fallback to reverse
    if (nCand == 0 && rev >= 0) {
      int nx = p.x + dirs[rev][0];
      int ny = p.y + dirs[rev][1];
      if (inBounds(nx, ny) && !posInList(nx, ny, occupied, nOccupied)) {
        bool blocked = false;
        for (int j = i + 1; j < nDetected; j++) {
          if (detected[j].x == nx && detected[j].y == ny) { blocked = true; break; }
        }
        if (!blocked) {
          candDirs[0] = rev;
          candPos[0] = {(uint8_t)nx, (uint8_t)ny};
          nCand = 1;
        }
      }
    }

    if (nCand == 0) {
      // Completely stuck — skip all moves this tick
      delay(MOVE_DELAY);
      return;
    }

    int pick = random(nCand);
    dest[i] = candPos[pick];
    chosenDir[i] = candDirs[pick];
    occupied[nOccupied++] = dest[i];
  }

  // ── Execute ──
  for (int i = 0; i < nDetected; i++) {
    Serial.printf("{\"type\":\"demo_random\",\"piece\":%d,\"from\":[%d,%d],\"to\":[%d,%d]}\n",
      i, detected[i].x, detected[i].y, dest[i].x, dest[i].y);
  }

  Board::MultiMoveRequest moves[DEMO_MAX_PIECES];
  for (int i = 0; i < nDetected; i++) {
    moves[i] = { detected[i].x, detected[i].y, dest[i].x, dest[i].y };
  }

  String result = board.moveMulti(moves, nDetected);
  Serial.println(result);

  if (result.indexOf("\"success\":true") >= 0) {
    for (int i = 0; i < nDetected; i++) {
      lastDirAt[dest[i].x / STEP][dest[i].y / STEP] = chosenDir[i];
      // Clear old direction memory
      if (detected[i].x != dest[i].x || detected[i].y != dest[i].y) {
        lastDirAt[detected[i].x / STEP][detected[i].y / STEP] = -1;
      }
    }
  }

  delay(MOVE_DELAY);
}
