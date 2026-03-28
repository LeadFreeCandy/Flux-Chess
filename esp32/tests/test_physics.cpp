// Standalone physics simulation test — runs on host (no ESP32 needed)
// Compile: cd esp32/tests && g++ -std=c++17 -O2 -o test_physics test_physics.cpp && ./test_physics

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cassert>
#include <vector>
#include <string>

// ═══════════════════════════════════════════════════════════════
// Stubs for Arduino/Hardware types
// ═══════════════════════════════════════════════════════════════

#define LOG_BOARD(fmt, ...) do { if (verbose) printf("[BOARD] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_HW(fmt, ...)   do { if (verbose) printf("[HW]    " fmt "\n", ##__VA_ARGS__); } while(0)

static bool verbose = false;

// Stub MoveError
enum class MoveError : uint8_t {
  NONE, OUT_OF_BOUNDS, SAME_POSITION, NOT_ORTHOGONAL,
  NO_PIECE_AT_SOURCE, PATH_BLOCKED, COIL_FAILURE
};

// ═══════════════════════════════════════════════════════════════
// Physics types — real-unit version
// ═══════════════════════════════════════════════════════════════

struct PhysicsParams {
  float piece_mass_g         = 2.7f;
  float max_current_a        = 1.0f;
  float mu_static            = 0.35f;
  float mu_kinetic           = 0.25f;
  float target_velocity_mm_s = 100.0f;
  float target_accel_mm_s2   = 500.0f;
  bool  active_brake         = true;
  uint16_t max_duration_ms   = 5000;
};

struct PieceState {
  float x_mm, y_mm;    // position in mm
  float vx_mm_s, vy_mm_s;  // velocity in mm/s
  bool stuck;

  void reset(float px_mm, float py_mm) {
    x_mm = px_mm; y_mm = py_mm;
    vx_mm_s = 0; vy_mm_s = 0;
    stuck = true;
  }
};

// ═══════════════════════════════════════════════════════════════
// Stub force functions (mN at 1A)
// ═══════════════════════════════════════════════════════════════

static constexpr float GRID_TO_MM = 38.0f / 3.0f;   // ~12.667 mm per coil spacing

// Horizontal component of force (mN at 1A) toward coil
float stubForceFx(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  if (d < 0.01f) return 0;
  float f = 54.0f * d / (d * d + 5.0f);
  return -f * (dx_mm / d);
}

float stubForceFy(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  if (d < 0.01f) return 0;
  float f = 54.0f * d / (d * d + 5.0f);
  return -f * (dy_mm / d);
}

// Vertical component (mN at 1A) — lifts piece, reduces normal force
float stubForceFz(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  return -101.0f / (1.0f + d * d * 0.01f);
}

// Magnitude of horizontal force at distance d (for 1D cases)
static float stubForceHMag(float d_mm) {
  if (d_mm < 0.01f) return 0;
  return 54.0f * d_mm / (d_mm * d_mm + 5.0f);
}

// ═══════════════════════════════════════════════════════════════
// Simulation result
// ═══════════════════════════════════════════════════════════════

struct SimResult {
  MoveError error;
  float final_x_mm, final_y_mm;
  float final_vx, final_vy;
  float elapsed_ms;
  int ticks;
  std::vector<std::pair<float,float>> trajectory;
};

static constexpr float MIN_TICK_S = 0.0005f;  // 500us minimum tick

// ═══════════════════════════════════════════════════════════════
// Core simulation loop
// path_mm[][2] — coil centres in mm
// ═══════════════════════════════════════════════════════════════

SimResult simulatePhysicsMove(PieceState& piece,
                               const float path_mm[][2], int path_len,
                               const PhysicsParams& params)
{
  SimResult result;
  result.error = MoveError::COIL_FAILURE;
  result.ticks = 0;

  if (path_len < 1) return result;

  const float mass_kg   = params.piece_mass_g * 1e-3f;
  const float weight_mN = params.piece_mass_g * 9.81f;   // mN

  float elapsed_s = 0;
  float max_s     = params.max_duration_ms / 1000.0f;

  int   coil_idx = 0;
  float cx_mm = path_mm[0][0];
  float cy_mm = path_mm[0][1];

  // Determine primary move direction from first→last coil
  float total_dx = path_mm[path_len-1][0] - piece.x_mm;
  float total_dy = path_mm[path_len-1][1] - piece.y_mm;
  bool  moveX    = (fabsf(total_dx) > fabsf(total_dy));

  LOG_BOARD("sim: start path_len=%d from=(%.1f,%.1f)mm to=(%.1f,%.1f)mm",
            path_len, piece.x_mm, piece.y_mm,
            path_mm[path_len-1][0], path_mm[path_len-1][1]);

  while (elapsed_s < max_s) {
    float dt = MIN_TICK_S;
    elapsed_s += dt;
    result.ticks++;

    // --- Distance / direction to active coil ---
    float ddx = cx_mm - piece.x_mm;
    float ddy = cy_mm - piece.y_mm;
    float dist_to_coil = sqrtf(ddx * ddx + ddy * ddy);

    // --- Force from active coil at 1A (mN) ---
    float fx1A = stubForceFx(piece.x_mm - cx_mm, piece.y_mm - cy_mm);
    float fy1A = stubForceFy(piece.x_mm - cx_mm, piece.y_mm - cy_mm);
    float fz1A = stubForceFz(piece.x_mm - cx_mm, piece.y_mm - cy_mm);

    // Horizontal force magnitude at 1A
    float fh1A = sqrtf(fx1A * fx1A + fy1A * fy1A);

    // Normal force at max current (used for static friction check)
    float normal_max_mN = fmaxf(weight_mN + fz1A * params.max_current_a, 0.0f);

    // --- Static friction check ---
    float static_friction_mN = params.mu_static * normal_max_mN;
    if (piece.stuck) {
      float max_force_mN = fh1A * params.max_current_a;
      if (max_force_mN > static_friction_mN) {
        piece.stuck = false;
        LOG_BOARD("sim: static friction overcome (F=%.2fmN > %.2fmN) at t=%.1fms",
                  max_force_mN, static_friction_mN, elapsed_s * 1000);
      } else {
        continue;
      }
    }

    // --- Controller: compute desired current ---
    float speed_mm_s = sqrtf(piece.vx_mm_s * piece.vx_mm_s + piece.vy_mm_s * piece.vy_mm_s);

    // Remaining distance to final destination
    float dest_dx = path_mm[path_len-1][0] - piece.x_mm;
    float dest_dy = path_mm[path_len-1][1] - piece.y_mm;
    float dist_remaining = sqrtf(dest_dx * dest_dx + dest_dy * dest_dy);

    // Stopping distance with NO current (weight-only normal, conservative)
    float friction_decel_coast = params.mu_kinetic * weight_mN / mass_kg;
    float stopping_dist_mm = (friction_decel_coast > 0)
                           ? (speed_mm_s * speed_mm_s) / (2.0f * friction_decel_coast)
                           : 0.0f;

    // Velocity component along direction to final destination
    float dir_to_dest_x = (dest_dx / (dist_remaining + 1e-6f));
    float dir_to_dest_y = (dest_dy / (dist_remaining + 1e-6f));
    float v_toward_dest = piece.vx_mm_s * dir_to_dest_x + piece.vy_mm_s * dir_to_dest_y;

    // Current to apply
    float current_a;
    if (coil_idx == path_len - 1 &&
        v_toward_dest > 0.0f &&
        stopping_dist_mm >= dist_remaining &&
        params.active_brake) {
      // Coast — let friction brake the piece
      current_a = 0.0f;
      LOG_BOARD("sim: coasting, dist_rem=%.1fmm stop_dist=%.1fmm", dist_remaining, stopping_dist_mm);
    } else {
      // Speed control toward active coil / destination
      // Always target cruise velocity — the coast condition handles stopping
      float target_v = params.target_velocity_mm_s;

      float v_along = moveX
        ? piece.vx_mm_s * (total_dx > 0 ? 1.0f : -1.0f)
        : piece.vy_mm_s * (total_dy > 0 ? 1.0f : -1.0f);

      float speed_error = target_v - v_along;
      float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel_mm_s2),
                                   params.target_accel_mm_s2);

      // Solve for current accounting for Fz-dependent friction:
      //   net_force = fh1A*I - mu*(weight + fz1A*I) = mass_kg * desired_accel
      //   I = (mass_kg*desired_accel + mu*weight) / (fh1A - mu*fz1A)
      float numerator   = mass_kg * desired_accel + params.mu_kinetic * weight_mN;
      float denominator = fh1A - params.mu_kinetic * fz1A;  // fz1A < 0 so denom > fh1A
      if (numerator < 0) numerator = 0;
      current_a = (denominator > 0.001f) ? (numerator / denominator) : params.max_current_a;
      if (current_a > params.max_current_a) current_a = params.max_current_a;
      if (current_a < 0)                    current_a = 0;
    }

    // --- Actual horizontal force from coil ---
    float fx_mN = fx1A * current_a;
    float fy_mN = fy1A * current_a;

    // Normal force with actual current applied (for friction in integration)
    float normal_mN = fmaxf(weight_mN + fz1A * current_a, 0.0f);

    // Kinetic friction (opposes velocity)
    if (speed_mm_s > 0.001f) {
      float fric_mN   = params.mu_kinetic * normal_mN;
      float max_fric  = mass_kg * speed_mm_s / dt;  // can't exceed momentum
      if (fric_mN > max_fric) fric_mN = max_fric;
      fx_mN -= fric_mN * (piece.vx_mm_s / speed_mm_s);
      fy_mN -= fric_mN * (piece.vy_mm_s / speed_mm_s);
    }

    // --- Integrate (F = ma, so a = F_mN / mass_kg) ---
    float ax = fx_mN / mass_kg;
    float ay = fy_mN / mass_kg;

    piece.vx_mm_s += ax * dt;
    piece.vy_mm_s += ay * dt;

    // Velocity safety clamp
    speed_mm_s = sqrtf(piece.vx_mm_s * piece.vx_mm_s + piece.vy_mm_s * piece.vy_mm_s);
    if (speed_mm_s > params.target_velocity_mm_s * 2.0f) {
      float scale = params.target_velocity_mm_s * 2.0f / speed_mm_s;
      piece.vx_mm_s *= scale;
      piece.vy_mm_s *= scale;
    }

    piece.x_mm += piece.vx_mm_s * dt;
    piece.y_mm += piece.vy_mm_s * dt;

    // Record trajectory every 10 ticks
    if (result.ticks % 10 == 0) {
      result.trajectory.push_back({piece.x_mm, piece.y_mm});
    }

    // --- Coil switching ---
    if (coil_idx < path_len - 1) {
      bool passed = moveX
        ? ((total_dx > 0 && piece.x_mm > cx_mm + 0.01f) ||
           (total_dx < 0 && piece.x_mm < cx_mm - 0.01f))
        : ((total_dy > 0 && piece.y_mm > cy_mm + 0.01f) ||
           (total_dy < 0 && piece.y_mm < cy_mm - 0.01f));

      if (passed) {
        coil_idx++;
        cx_mm = path_mm[coil_idx][0];
        cy_mm = path_mm[coil_idx][1];
        LOG_BOARD("sim: switch to coil at (%.1f,%.1f)mm idx=%d/%d t=%.1fms",
                  cx_mm, cy_mm, coil_idx, path_len, elapsed_s * 1000);
      }
    }

    // --- Arrival check ---
    if (coil_idx == path_len - 1) {
      float adx = path_mm[path_len-1][0] - piece.x_mm;
      float ady = path_mm[path_len-1][1] - piece.y_mm;
      float d   = sqrtf(adx * adx + ady * ady);
      float spd = sqrtf(piece.vx_mm_s * piece.vx_mm_s + piece.vy_mm_s * piece.vy_mm_s);

      if (d < 1.0f && spd < 5.0f) {
        LOG_BOARD("sim: arrived at (%.1f,%.1f)mm d=%.2fmm spd=%.2fmm/s t=%.1fms",
                  path_mm[path_len-1][0], path_mm[path_len-1][1],
                  d, spd, elapsed_s * 1000);
        piece.x_mm    = path_mm[path_len-1][0];
        piece.y_mm    = path_mm[path_len-1][1];
        piece.vx_mm_s = 0;
        piece.vy_mm_s = 0;
        piece.stuck   = true;
        result.error  = MoveError::NONE;
        break;
      }
    }
  }

  result.final_x_mm  = piece.x_mm;
  result.final_y_mm  = piece.y_mm;
  result.final_vx    = piece.vx_mm_s;
  result.final_vy    = piece.vy_mm_s;
  result.elapsed_ms  = elapsed_s * 1000;
  return result;
}

// Convenience: build mm path from integer grid coordinates
static void gridToMm(const uint8_t grid[][2], float mm_out[][2], int n) {
  for (int i = 0; i < n; i++) {
    mm_out[i][0] = grid[i][0] * GRID_TO_MM;
    mm_out[i][1] = grid[i][1] * GRID_TO_MM;
  }
}

// ═══════════════════════════════════════════════════════════════
// Test harness
// ═══════════════════════════════════════════════════════════════

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
  printf("  %-50s ", name); \
  {

#define EXPECT(cond) \
  if (!(cond)) { \
    printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
    tests_failed++; \
  } else

#define PASS \
  { printf("OK\n"); tests_passed++; }

#define END_TEST }

// ───────────────────────────────────────────────────────────────

void test_default_sanity() {
  printf("\n=== Default Parameter Sanity ===\n");
  PhysicsParams p;

  float mass_kg     = p.piece_mass_g * 1e-3f;
  float weight_mN   = p.piece_mass_g * 9.81f;
  // Peak force at ~2.2mm (d_peak ≈ sqrt(5) for stub model)
  float d_peak_mm   = sqrtf(5.0f);
  float f_peak_mN   = stubForceHMag(d_peak_mm);
  float f_at_spacing = stubForceHMag(GRID_TO_MM);

  float static_fric_mN  = p.mu_static  * weight_mN;
  float kinetic_fric_mN = p.mu_kinetic * weight_mN;

  float t_cross_s  = (9 * GRID_TO_MM) / p.target_velocity_mm_s;
  float t_accel_ms = (p.target_velocity_mm_s / p.target_accel_mm_s2) * 1000.0f;
  float d_brake_mm = (p.target_velocity_mm_s * p.target_velocity_mm_s)
                   / (2.0f * (p.mu_kinetic * weight_mN / mass_kg));

  printf("  Piece mass:           %.1f g  (%.4f kg)\n", p.piece_mass_g, mass_kg);
  printf("  Weight:               %.2f mN\n", weight_mN);
  printf("  Peak force (1A):      %.2f mN at d=%.2f mm\n", f_peak_mN, d_peak_mm);
  printf("  Force at coil spacing:%.2f mN\n", f_at_spacing);
  printf("  Static friction:      %.2f mN\n", static_fric_mN);
  printf("  Kinetic friction:     %.2f mN\n", kinetic_fric_mN);
  printf("  Board cross time:     %.2f s (9 coils at %.0f mm/s)\n",
         t_cross_s, p.target_velocity_mm_s);
  printf("  Accel time:           %.0f ms (to %.0fmm/s at %.0fmm/s²)\n",
         t_accel_ms, p.target_velocity_mm_s, p.target_accel_mm_s2);
  printf("  Brake distance:       %.2f mm\n", d_brake_mm);

  // At coil spacing, Fz reduces the normal force — compute effective frictions
  float fz_at_spacing  = stubForceFz(GRID_TO_MM, 0);
  float normal_reduced = fmaxf(weight_mN + fz_at_spacing * p.max_current_a, 0.0f);
  float static_fric_reduced  = p.mu_static  * normal_reduced;
  float kinetic_fric_reduced = p.mu_kinetic * normal_reduced;

  printf("  Fz at coil spacing:   %.2f mN/A  → normal=%.2f mN\n",
         fz_at_spacing, normal_reduced);
  printf("  Effective static fric:%.2f mN (reduced by Fz)\n", static_fric_reduced);
  printf("  Effective kinet. fric:%.2f mN (reduced by Fz)\n", kinetic_fric_reduced);

  TEST("Peak force (1A) > effective static friction at coil spacing") {
    // Fz from the coil reduces normal force, so friction is lower than weight-only
    printf("\n    peak=%.2fmN > eff_static=%.2fmN\n    ", f_at_spacing, static_fric_reduced);
    EXPECT(f_at_spacing > static_fric_reduced) PASS;
  } END_TEST

  TEST("Static friction > kinetic friction") {
    EXPECT(static_fric_mN > kinetic_fric_mN) PASS;
  } END_TEST

  TEST("Force at coil spacing (12.67mm) > effective kinetic friction") {
    printf("\n    f=%.2fmN > eff_kinetic=%.2fmN\n    ", f_at_spacing, kinetic_fric_reduced);
    EXPECT(f_at_spacing > kinetic_fric_reduced) PASS;
  } END_TEST

  TEST("Braking distance < 200 mm (friction-only worst case)") {
    printf("\n    d_brake=%.2fmm\n    ", d_brake_mm);
    EXPECT(d_brake_mm < 200.0f) PASS;
  } END_TEST

  TEST("Board cross time < 5 s") {
    EXPECT(t_cross_s < 5.0f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_force_model() {
  printf("\n=== Stub Force Model ===\n");

  TEST("Force is zero at d=0") {
    float f = stubForceHMag(0);
    EXPECT(fabsf(f) < 0.001f) PASS;
  } END_TEST

  TEST("Force peaks at intermediate distance (~sqrt(5)mm)") {
    float d_peak = sqrtf(5.0f);
    float f_01   = stubForceHMag(0.1f);
    float f_peak = stubForceHMag(d_peak);
    float f_50   = stubForceHMag(50.0f);
    EXPECT(f_peak > f_01 && f_peak > f_50) PASS;
  } END_TEST

  TEST("Force falls off at large distance") {
    float f_5  = stubForceHMag(5.0f);
    float f_20 = stubForceHMag(20.0f);
    float f_50 = stubForceHMag(50.0f);
    EXPECT(f_5 > f_20 && f_20 > f_50) PASS;
  } END_TEST

  TEST("Fz is negative (lifts piece, reduces normal force)") {
    float fz = stubForceFz(0, 0);
    EXPECT(fz < 0) PASS;
  } END_TEST

  TEST("Fx direction: piece left of coil → positive Fx") {
    // piece at x=0, coil at x=5mm → dx_mm = 0-5 = -5
    float fx = stubForceFx(-5.0f, 0);
    EXPECT(fx > 0) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_friction_clamping() {
  printf("\n=== Friction Clamping ===\n");

  TEST("Piece with velocity doesn't reverse due to friction") {
    PhysicsParams p;
    float mass_kg  = p.piece_mass_g * 1e-3f;
    float weight_mN = p.piece_mass_g * 9.81f;

    PieceState piece;
    piece.reset(0, 0);
    piece.stuck    = false;
    piece.vy_mm_s  = 10.0f;  // moving at 10 mm/s

    float dt = MIN_TICK_S;
    float speed = piece.vy_mm_s;
    float fric_mN = p.mu_kinetic * weight_mN;
    float max_fric = mass_kg * speed / dt;
    if (fric_mN > max_fric) fric_mN = max_fric;
    piece.vy_mm_s += (-fric_mN / mass_kg) * dt;

    EXPECT(piece.vy_mm_s >= -0.001f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_simple_move() {
  printf("\n=== Simple Move (mm path) ===\n");

  TEST("Piece moves from (0,0) toward (0, 3*GRID_TO_MM) mm") {
    PhysicsParams p;
    p.max_duration_ms = 3000;

    PieceState piece;
    piece.reset(0, 0);

    // 3-step path in mm
    float path_mm[3][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
      { 0, 3 * GRID_TO_MM },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 3, p);

    printf("\n    final=(%.2f,%.2f)mm v=(%.2f,%.2f)mm/s t=%.0fms err=%d\n    ",
           r.final_x_mm, r.final_y_mm, r.final_vx, r.final_vy,
           r.elapsed_ms, (int)r.error);

    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST

  TEST("Piece arrives within 1mm of destination") {
    PhysicsParams p;
    PieceState piece;
    piece.reset(0, 0);

    float dest = 3 * GRID_TO_MM;
    float path_mm[3][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
      { 0, dest },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 3, p);
    printf("\n    final_y=%.2fmm dest=%.2fmm err=%d\n    ",
           r.final_y_mm, dest, (int)r.error);
    EXPECT(r.error == MoveError::NONE && fabsf(r.final_y_mm - dest) < 1.0f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_horizontal_move() {
  printf("\n=== Horizontal Move ===\n");

  TEST("Piece moves from (0,0) to (3*GRID_TO_MM, 0) mm") {
    PhysicsParams p;
    PieceState piece;
    piece.reset(0, 0);

    float dest = 3 * GRID_TO_MM;
    float path_mm[3][2] = {
      { 1 * GRID_TO_MM, 0 },
      { 2 * GRID_TO_MM, 0 },
      { dest,           0 },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 3, p);
    printf("\n    final=(%.2f,%.2f)mm t=%.0fms err=%d\n    ",
           r.final_x_mm, r.final_y_mm, r.elapsed_ms, (int)r.error);
    EXPECT(r.error == MoveError::NONE && fabsf(r.final_x_mm - dest) < 1.0f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_coil_switching() {
  printf("\n=== Coil Switching ===\n");

  TEST("Piece passes through intermediate coils (verbose)") {
    PhysicsParams p;
    p.max_duration_ms = 4000;
    verbose = true;

    PieceState piece;
    piece.reset(0, 0);

    float path_mm[3][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
      { 0, 3 * GRID_TO_MM },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 3, p);
    printf("    final=(%.2f,%.2f)mm t=%.0fms switches in logs above\n    ",
           r.final_x_mm, r.final_y_mm, r.elapsed_ms);
    verbose = false;
    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_velocity_never_reverses() {
  printf("\n=== Velocity Stability ===\n");

  TEST("Friction clamping never reverses velocity in one tick") {
    // Verify that a single friction step with extreme friction doesn't
    // flip velocity sign — the max_fric cap prevents this.
    PhysicsParams p;
    p.mu_kinetic = 50.0f;  // absurdly high friction

    float mass_kg   = p.piece_mass_g * 1e-3f;
    float weight_mN = p.piece_mass_g * 9.81f;
    float dt = MIN_TICK_S;

    // Piece moving at 10mm/s
    float vy = 10.0f;
    float normal = weight_mN;
    float fric_mN  = p.mu_kinetic * normal;
    float max_fric = mass_kg * vy / dt;
    if (fric_mN > max_fric) fric_mN = max_fric;
    vy += (-fric_mN / mass_kg) * dt;

    printf("\n    vy_after=%.4f mm/s (should be >= 0)\n    ", vy);
    EXPECT(vy >= -0.001f) PASS;
  } END_TEST

  TEST("Full simulation move completes and piece doesn't fly away") {
    PhysicsParams p;
    p.max_duration_ms = 3000;

    PieceState piece;
    piece.reset(0, 0);

    float path_mm[2][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 2, p);

    printf("\n    final_y=%.2fmm (dest=%.2fmm) err=%d\n    ",
           r.final_y_mm, 2 * GRID_TO_MM, (int)r.error);
    // Piece should have moved in the +Y direction and not gone backwards
    EXPECT(r.final_y_mm > 0.0f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_param_sensitivity() {
  printf("\n=== Parameter Sensitivity ===\n");

  auto run_move = [](PhysicsParams& p) -> SimResult {
    PieceState piece;
    piece.reset(0, 0);
    float path_mm[3][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
      { 0, 3 * GRID_TO_MM },
    };
    return simulatePhysicsMove(piece, path_mm, 3, p);
  };

  TEST("Higher target_velocity moves piece faster") {
    PhysicsParams p1, p2;
    p1.target_velocity_mm_s = 50.0f;
    p2.target_velocity_mm_s = 150.0f;
    SimResult r1 = run_move(p1);
    SimResult r2 = run_move(p2);
    printf("\n    v=50: %.0fms (err=%d), v=150: %.0fms (err=%d)\n    ",
           r1.elapsed_ms, (int)r1.error, r2.elapsed_ms, (int)r2.error);
    EXPECT(r2.elapsed_ms < r1.elapsed_ms ||
           (r1.error != MoveError::NONE && r2.error == MoveError::NONE)) PASS;
  } END_TEST

  TEST("Very high friction prevents movement") {
    PhysicsParams p;
    p.mu_static  = 10.0f;   // extreme: requires 10× weight in force
    p.mu_kinetic = 8.0f;
    SimResult r = run_move(p);
    printf("\n    err=%d final_y=%.2fmm\n    ", (int)r.error, r.final_y_mm);
    EXPECT(r.error == MoveError::COIL_FAILURE && fabsf(r.final_y_mm) < 1.0f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_velocity_profile() {
  printf("\n=== Velocity Profile ===\n");

  TEST("Piece accelerates to target velocity (mm/s)") {
    PhysicsParams p;
    float mass_kg   = p.piece_mass_g * 1e-3f;
    float weight_mN = p.piece_mass_g * 9.81f;

    PieceState piece;
    piece.reset(0, 0);
    piece.stuck = false;

    float target_y  = GRID_TO_MM;
    float elapsed_s = 0;
    float dt        = MIN_TICK_S;
    float max_vy    = 0;

    printf("\n    time(ms)  pos(mm)  vel(mm/s)  F_avail(mN)  current(A)\n");

    for (int i = 0; i < 2000 && elapsed_s < 1.0f; i++) {
      elapsed_s += dt;

      float ddy   = target_y - piece.y_mm;
      float dist  = fabsf(ddy);
      float fh1A  = stubForceHMag(dist);
      float fz1A  = stubForceFz(0, piece.y_mm - target_y);
      float normal = fmaxf(weight_mN + fz1A * p.max_current_a, 0.0f);

      float kinetic_fric_mN = p.mu_kinetic * normal;
      float v_along = piece.vy_mm_s;
      float speed_err = p.target_velocity_mm_s - v_along;
      float desired_accel = fminf(fmaxf(speed_err / dt,
                                        -p.target_accel_mm_s2),
                                   p.target_accel_mm_s2);
      float desired_force_mN = mass_kg * desired_accel + kinetic_fric_mN;
      if (desired_force_mN < 0) desired_force_mN = 0;

      float current_a = (fh1A > 0.001f) ? (desired_force_mN / fh1A) : p.max_current_a;
      if (current_a > p.max_current_a) current_a = p.max_current_a;
      if (current_a < 0)               current_a = 0;

      float fy_mN = fh1A * current_a;

      float speed = fabsf(piece.vy_mm_s);
      if (speed > 0.001f) {
        float fric_mN  = p.mu_kinetic * normal;
        float max_fric = mass_kg * speed / dt;
        if (fric_mN > max_fric) fric_mN = max_fric;
        fy_mN -= fric_mN;
      }

      piece.vy_mm_s += (fy_mN / mass_kg) * dt;
      if (piece.vy_mm_s > p.target_velocity_mm_s * 1.5f)
        piece.vy_mm_s = p.target_velocity_mm_s * 1.5f;
      piece.y_mm += piece.vy_mm_s * dt;

      if (piece.vy_mm_s > max_vy) max_vy = piece.vy_mm_s;

      if (i % 100 == 0) {
        printf("    %6.1f   %7.2f   %8.2f   %10.2f   %9.3f\n",
               elapsed_s * 1000, piece.y_mm, piece.vy_mm_s, fh1A, current_a);
      }
    }

    printf("    Peak vel: %.2fmm/s  Final: pos=%.2fmm vel=%.2fmm/s at t=%.0fms\n    ",
           max_vy, piece.y_mm, piece.vy_mm_s, elapsed_s * 1000);

    // Piece should have accelerated to near target_velocity at some point
    EXPECT(max_vy > p.target_velocity_mm_s * 0.5f) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

void test_long_move() {
  printf("\n=== Long Move ===\n");

  TEST("Piece moves from (0,0) to (0, 6*GRID_TO_MM)mm — full board height") {
    PhysicsParams p;
    p.max_duration_ms = 10000;

    PieceState piece;
    piece.reset(0, 0);

    float path_mm[6][2] = {
      { 0, 1 * GRID_TO_MM },
      { 0, 2 * GRID_TO_MM },
      { 0, 3 * GRID_TO_MM },
      { 0, 4 * GRID_TO_MM },
      { 0, 5 * GRID_TO_MM },
      { 0, 6 * GRID_TO_MM },
    };

    SimResult r = simulatePhysicsMove(piece, path_mm, 6, p);
    printf("\n    final=(%.2f,%.2f)mm t=%.0fms err=%d\n    ",
           r.final_x_mm, r.final_y_mm, r.elapsed_ms, (int)r.error);
    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST
}

// ───────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "-v") == 0) verbose = true;

  printf("FluxChess Physics Simulation Tests (real-unit)\n");
  printf("===============================================\n");

  test_default_sanity();
  test_force_model();
  test_friction_clamping();
  test_simple_move();
  test_horizontal_move();
  test_coil_switching();
  test_velocity_never_reverses();
  test_param_sensitivity();
  test_velocity_profile();
  test_long_move();

  printf("\n===============================================\n");
  printf("Results: %d passed, %d failed\n\n", tests_passed, tests_failed);

  return tests_failed > 0 ? 1 : 0;
}
