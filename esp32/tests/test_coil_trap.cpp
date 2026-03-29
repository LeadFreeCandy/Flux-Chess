// Test for coil center trapping bug — uses REAL force tables, not stubs.
// This should reproduce the exact firmware behavior.
//
// Compile: g++ -std=c++17 -O2 -o test_coil_trap test_coil_trap.cpp && ./test_coil_trap

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <initializer_list>

// Include the REAL force tables from firmware
#include "../firmware/force_tables.h"

static constexpr float GRID_TO_MM = 38.0f / 3.0f; // 12.667mm

// ═══════════════════════════════════════════════════════════════
// Bilinear interpolation — identical to firmware physics.h
// ═══════════════════════════════════════════════════════════════

float tableForceFx(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fx[layer][iy][ix]
       + tx*(1-ty)*force_table_fx[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fx[layer][iy+1][ix]
       + tx*ty*force_table_fx[layer][iy+1][ix+1];
}

float tableForceFy(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fy[layer][iy][ix]
       + tx*(1-ty)*force_table_fy[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fy[layer][iy+1][ix]
       + tx*ty*force_table_fy[layer][iy+1][ix+1];
}

float tableForceFz(int layer, float dx_mm, float dy_mm) {
  if (layer < 0 || layer >= FORCE_TABLE_NUM_LAYERS) return 0;
  float fx = (dx_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  float fy = (dy_mm + FORCE_TABLE_EXTENT_MM) / FORCE_TABLE_RES_MM;
  int ix = (int)fx, iy = (int)fy;
  if (ix < 0 || ix >= FORCE_TABLE_SIZE - 1 || iy < 0 || iy >= FORCE_TABLE_SIZE - 1) return 0;
  float tx = fx - ix, ty = fy - iy;
  return (1-tx)*(1-ty)*force_table_fz[layer][iy][ix]
       + tx*(1-ty)*force_table_fz[layer][iy][ix+1]
       + (1-tx)*ty*force_table_fz[layer][iy+1][ix]
       + tx*ty*force_table_fz[layer][iy+1][ix+1];
}

// ═══════════════════════════════════════════════════════════════
// Types — match firmware exactly
// ═══════════════════════════════════════════════════════════════

enum class MoveError : uint8_t {
  NONE, OUT_OF_BOUNDS, SAME_POSITION, NOT_ORTHOGONAL,
  NO_PIECE_AT_SOURCE, PATH_BLOCKED, COIL_FAILURE
};

struct PieceState {
  float x, y, vx, vy;
  bool stuck;
  void reset(float px, float py) { x = px; y = py; vx = 0; vy = 0; stuck = true; }
};

struct PhysicsParams {
  float piece_mass_g         = 2.7f;
  float max_current_a        = 2.0f;
  float mu_static            = 0.55f;
  float mu_kinetic           = 0.45f;
  float target_velocity_mm_s = 50.0f;
  float target_accel_mm_s2   = 500.0f;
  float max_jerk_mm_s3       = 50000.0f;
  float coast_friction_offset = 0.0f;
  uint16_t brake_pulse_ms    = 100;
  float pwm_compensation     = 0.0f;
  bool  all_coils_equal      = true;
  float force_scale          = 1.0f;
  uint16_t max_duration_ms   = 5000;
};

// ═══════════════════════════════════════════════════════════════
// Simulation — mirrors firmware physics.h execute() exactly
// ═══════════════════════════════════════════════════════════════

struct SimResult {
  MoveError error;
  float final_x, final_y;
  float elapsed_ms;
  float max_speed;
  int coil_switches;
  float stuck_at_mm;
};

SimResult simulate(PieceState& piece, const float path_mm[][2], int path_len,
                   const PhysicsParams& params, bool verbose = false)
{
  SimResult result{};
  result.error = MoveError::COIL_FAILURE;
  result.stuck_at_mm = -1;

  if (path_len < 1 || params.piece_mass_g <= 0 || params.max_current_a <= 0 || params.target_velocity_mm_s <= 0)
    return result;

  int coil_idx = 0;
  float cx = path_mm[0][0], cy = path_mm[0][1];
  float dx = path_mm[path_len-1][0] - piece.x;
  float dy = path_mm[path_len-1][1] - piece.y;
  bool moveX = (fabsf(dx) > fabsf(dy));
  float move_sign = moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f);

  int activeLayer = 0; // all_coils_equal

  float weight_mN = params.piece_mass_g * 9.81f;
  float mass_kg = params.piece_mass_g * 1e-3f;

  uint8_t last_duty = 255;
  float last_current = params.max_current_a;
  float max_speed = 0;
  bool coasting = false;
  bool centered = false;
  int coil_switches = 0;

  static constexpr float COAST_TOLERANCE_MM = 3.0f;
  static constexpr float ARRIVAL_DIST_MM = 1.0f;
  static constexpr float ARRIVAL_SPEED_MM_S = 5.0f;
  static constexpr float CENTERED_SPEED_MM_S = 10.0f;
  static constexpr float SPEED_CLAMP_FACTOR = 1.5f;

  float dt = 0.001f;  // 1ms tick, matches firmware
  float elapsed_s = 0;
  float max_s = params.max_duration_ms / 1000.0f;

  // Detect stuck: track how long piece stays near a coil center
  int near_center_ticks = 0;

  while (elapsed_s < max_s) {
    elapsed_s += dt;

    float off_x = piece.x - cx;
    float off_y = piece.y - cy;

    // Force table lookup — REAL tables, same as firmware
    float fx_1a = tableForceFx(activeLayer, off_x, off_y) * params.force_scale;
    float fy_1a = tableForceFy(activeLayer, off_x, off_y) * params.force_scale;
    float fz_1a = tableForceFz(activeLayer, off_x, off_y) * params.force_scale;

    float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
    float v_along = (moveX ? piece.vx : piece.vy) * move_sign;

    // 4. Dynamic friction
    float normal_mN = weight_mN - fz_1a * last_current;
    if (normal_mN < 0) normal_mN = 0;
    float mu = (piece.stuck || speed < 0.1f) ? params.mu_static : params.mu_kinetic;
    float friction_mN = mu * normal_mN;

    // 5. Static friction check
    if (piece.stuck) {
      float avail = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a) * params.max_current_a;
      float static_fric = params.mu_static * fmaxf(weight_mN - fz_1a * params.max_current_a, 0.0f);
      if (avail > fmaxf(static_fric, 0)) {
        piece.stuck = false;
      } else {
        continue;
      }
    }

    float fx, fy;
    uint8_t duty;

    if (!coasting) {
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float dist_remain = moveX ? (dest_x - piece.x) * move_sign : (dest_y - piece.y) * move_sign;
      float coast_mu = params.mu_kinetic + params.coast_friction_offset;
      float friction_decel = (weight_mN > 0) ? coast_mu * weight_mN / mass_kg : 0;
      float stopping_dist = (friction_decel > 0.01f) ? (v_along * v_along) / (2.0f * friction_decel) : 9999.0f;

      if (v_along > 0 && dist_remain > 0 && stopping_dist >= dist_remain) {
        coasting = true;
        last_current = 0;
        normal_mN = weight_mN;
      }
    }

    if (coasting) {
      fx = 0; fy = 0; duty = 0;
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));
      if (!centered && params.brake_pulse_ms > 0 && d_dest < COAST_TOLERANCE_MM && speed < 20.0f) {
        centered = true;
      }
    } else {
      // 7. Controller — no jerk limiting, 12-bit duty
      float speed_error = params.target_velocity_mm_s - v_along;
      float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel_mm_s2), params.target_accel_mm_s2);

      float desired_force = mass_kg * desired_accel + friction_mN;
      if (desired_force < 0) desired_force = 0;

      float avail_lateral = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a);
      float required_current = (avail_lateral > 0.01f) ? desired_force / avail_lateral : params.max_current_a;
      required_current = fminf(required_current, params.max_current_a);
      if (required_current < 0) required_current = 0;

      float desired_eff_duty = required_current / params.max_current_a * 255.0f;
      float comp = params.pwm_compensation;
      float raw_duty = (comp < 0.99f) ? (desired_eff_duty - 255.0f * comp) / (1.0f - comp) : 0;
      if (raw_duty < 0) raw_duty = 0;
      if (raw_duty > 255) raw_duty = 255;
      duty = (uint8_t)raw_duty;
      if (duty == 0 && desired_accel > 0 && desired_force > 0) duty = 1;
      float eff_duty = (duty > 0) ? duty + (255.0f - duty) * comp : 0;
      float actual_current = (eff_duty / 255.0f) * params.max_current_a;
      last_current = actual_current;

      fx = fx_1a * actual_current;
      fy = fy_1a * actual_current;
    }

    // 8. Friction
    if (speed > 0.1f) {
      float fric = params.mu_kinetic * fmaxf(normal_mN, 0);
      float max_fric = speed / dt * mass_kg;
      if (fric > max_fric) fric = max_fric;
      fx -= fric * (piece.vx / speed);
      fy -= fric * (piece.vy / speed);
    }

    // 9. Update velocity
    piece.vx += (fx / mass_kg) * dt;
    piece.vy += (fy / mass_kg) * dt;

    speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
    if (speed > params.target_velocity_mm_s * SPEED_CLAMP_FACTOR) {
      float scale = params.target_velocity_mm_s * SPEED_CLAMP_FACTOR / speed;
      piece.vx *= scale; piece.vy *= scale;
    }
    if (speed > max_speed) max_speed = speed;

    // 10. Update position
    piece.x += piece.vx * dt;
    piece.y += piece.vy * dt;

    // Verbose logging — same format as firmware
    if (verbose && (int)(elapsed_s * 100) % 10 == 0) {
      float dist_to_coil = sqrtf(off_x * off_x + off_y * off_y);
      printf("  t=%6.0fms pos=(%.1f,%.1f) v=(%.1f,%.1f) duty=%d coil=%d d_coil=%.1f Fh=%.1f Fz=%.1f N=%.1f fric=%.1f %s\n",
             elapsed_s*1000, piece.x, piece.y, piece.vx, piece.vy, duty,
             coil_idx, dist_to_coil,
             sqrtf(fx_1a*fx_1a + fy_1a*fy_1a) * last_current,
             fz_1a * last_current, normal_mN, friction_mN,
             coasting ? "COAST" : "");
    }

    // Detect stuck
    float dist_to_coil = sqrtf(off_x * off_x + off_y * off_y);
    if (dist_to_coil < 2.0f && speed < 15.0f && !coasting) {
      near_center_ticks++;
      if (near_center_ticks == 100 && result.stuck_at_mm < 0) { // 1s stuck
        result.stuck_at_mm = moveX ? piece.x : piece.y;
        if (verbose) printf("  >>> STUCK at %.1fmm (coil center at %.1f)\n", result.stuck_at_mm, cx);
      }
    } else {
      near_center_ticks = 0;
    }

    // 11. Coil switching — pick whichever coil produces greater forward acceleration
    if (!coasting && coil_idx < path_len - 1) {
      float next_cx = path_mm[coil_idx + 1][0];
      float next_cy = path_mm[coil_idx + 1][1];
      float next_off_x = piece.x - next_cx;
      float next_off_y = piece.y - next_cy;

      // Current coil: net forward acceleration
      float cur_fh = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a); // already computed above at 1A
      float cur_fz = fz_1a; // already computed
      float cur_lateral = cur_fh * params.max_current_a;
      float cur_normal = fmaxf(weight_mN - cur_fz * params.max_current_a, 0.0f);
      float cur_friction = params.mu_kinetic * cur_normal;
      // Project lateral force onto move direction
      float cur_fx_move = moveX ? fx_1a : fy_1a;
      float cur_forward = cur_fx_move * move_sign * params.max_current_a;
      float cur_net_accel = (cur_forward - cur_friction) / mass_kg;

      // Next coil: net forward acceleration
      float next_fx_1a = tableForceFx(0, next_off_x, next_off_y) * params.force_scale;
      float next_fy_1a = tableForceFy(0, next_off_x, next_off_y) * params.force_scale;
      float next_fz_1a = tableForceFz(0, next_off_x, next_off_y) * params.force_scale;
      float next_fx_move = moveX ? next_fx_1a : next_fy_1a;
      float next_forward = next_fx_move * move_sign * params.max_current_a;
      float next_normal = fmaxf(weight_mN - next_fz_1a * params.max_current_a, 0.0f);
      float next_friction = params.mu_kinetic * next_normal;
      float next_net_accel = (next_forward - next_friction) / mass_kg;

      if (next_net_accel > cur_net_accel) {
        coil_idx++;
        cx = next_cx;
        cy = next_cy;
        activeLayer = 0;
        last_duty = 255;
        last_current = params.max_current_a;
        coil_switches++;
        if (verbose) printf("  >>> SWITCH coil %d at (%.1f,%.1f) cur_accel=%.0f next_accel=%.0f\n",
                            coil_idx, cx, cy, cur_net_accel, next_net_accel);
      }
    }

    // 13. Arrival check
    {
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));
      bool arrived = (centered && speed < CENTERED_SPEED_MM_S) || (d_dest < ARRIVAL_DIST_MM && speed < ARRIVAL_SPEED_MM_S);
      if (arrived) {
        piece.x = dest_x; piece.y = dest_y;
        piece.vx = 0; piece.vy = 0;
        result.error = MoveError::NONE;
        break;
      }
    }
  }

  result.final_x = piece.x;
  result.final_y = piece.y;
  result.elapsed_ms = elapsed_s * 1000;
  result.max_speed = max_speed;
  result.coil_switches = coil_switches;
  return result;
}

// ═══════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0) verbose = true;
  }

  printf("=== Coil Trap Test (REAL force tables) ===\n\n");

  PhysicsParams p;
  p.piece_mass_g = 2.7f;
  p.max_current_a = 2.0f;
  p.mu_static = 0.55f;
  p.mu_kinetic = 0.45f;
  p.target_velocity_mm_s = 50.0f;
  p.target_accel_mm_s2 = 500.0f;
  p.max_jerk_mm_s3 = 50000.0f;
  p.pwm_compensation = 0.0f;
  p.all_coils_equal = true;
  p.force_scale = 1.0f;
  p.max_duration_ms = 5000;

  // ── Force profile at coil center (real tables) ──
  printf("--- Real force table profile at coil center (layer 0) ---\n");
  printf("  %-8s  %-10s  %-10s  %-10s  %-10s  %-10s\n",
         "offset", "Fx mN/A", "Fz mN/A", "normal", "friction", "net @2A");
  float weight = p.piece_mass_g * 9.81f;
  for (float d = -3.0f; d <= 3.0f; d += 0.5f) {
    float fx = tableForceFx(0, d, 0);
    float fz = tableForceFz(0, d, 0);
    float normal = fmaxf(weight - fz * p.max_current_a, 0.0f);
    float friction = p.mu_kinetic * normal;
    float net = fabsf(fx) * p.max_current_a - friction;
    printf("  %+5.1fmm   %+8.2f   %+8.2f   %8.2f   %8.2f   %+8.2f %s\n",
           d, fx, fz, normal, friction, net, net <= 0 ? "TRAPPED" : "");
  }

  // ── 6-step forward move ──
  printf("\n--- 6-step forward: (0,3) -> (6,3) ---\n");
  {
    float path[6][2];
    for (int i = 0; i < 6; i++) { path[i][0] = (i+1) * GRID_TO_MM; path[i][1] = 3 * GRID_TO_MM; }

    PieceState piece; piece.reset(0, 3 * GRID_TO_MM);
    SimResult r = simulate(piece, path, 6, p, verbose);
    printf("  %s  final=(%.1f,%.1f) t=%.0fms max_v=%.0f switches=%d stuck_at=%.1f\n",
           r.error == MoveError::NONE ? "OK" : "FAIL/TIMEOUT",
           r.final_x, r.final_y, r.elapsed_ms, r.max_speed, r.coil_switches, r.stuck_at_mm);
  }

  // ── 6-step return move ──
  printf("\n--- 6-step return: (6,3) -> (0,3) ---\n");
  {
    float rpath[6][2];
    for (int i = 0; i < 6; i++) { rpath[i][0] = (5-i) * GRID_TO_MM; rpath[i][1] = 3 * GRID_TO_MM; }

    PieceState piece; piece.reset(6 * GRID_TO_MM, 3 * GRID_TO_MM);
    SimResult r = simulate(piece, rpath, 6, p, verbose);
    printf("  %s  final=(%.1f,%.1f) t=%.0fms max_v=%.0f switches=%d stuck_at=%.1f\n",
           r.error == MoveError::NONE ? "OK" : "FAIL/TIMEOUT",
           r.final_x, r.final_y, r.elapsed_ms, r.max_speed, r.coil_switches, r.stuck_at_mm);
  }

  // ── Velocity sweep ──
  printf("\n--- Velocity sweep (6-step, I=2A, accel=500) ---\n");
  printf("  %-8s  %-10s  %-10s  %-8s  %-10s\n", "v mm/s", "result", "time_ms", "switches", "stuck_at");
  for (float v : {25.0f, 50.0f, 75.0f, 100.0f, 150.0f, 200.0f, 300.0f}) {
    p.target_velocity_mm_s = v;

    float path[6][2];
    for (int i = 0; i < 6; i++) { path[i][0] = (i+1) * GRID_TO_MM; path[i][1] = 0; }

    PieceState piece; piece.reset(0, 0);
    SimResult r = simulate(piece, path, 6, p);
    printf("  %6.0f    %-10s  %7.0f    %d         %.1f\n",
           v, r.error == MoveError::NONE ? "OK" : "STUCK",
           r.elapsed_ms, r.coil_switches, r.stuck_at_mm);
  }

  // ── Current sweep ──
  p.target_velocity_mm_s = 50.0f;
  printf("\n--- Current sweep (6-step, v=50, accel=500) ---\n");
  printf("  %-8s  %-10s  %-10s  %-8s  %-10s\n", "I (A)", "result", "time_ms", "switches", "stuck_at");
  for (float I : {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f}) {
    p.max_current_a = I;

    float path[6][2];
    for (int i = 0; i < 6; i++) { path[i][0] = (i+1) * GRID_TO_MM; path[i][1] = 0; }

    PieceState piece; piece.reset(0, 0);
    SimResult r = simulate(piece, path, 6, p);
    printf("  %5.1f     %-10s  %7.0f    %d         %.1f\n",
           I, r.error == MoveError::NONE ? "OK" : "STUCK",
           r.elapsed_ms, r.coil_switches, r.stuck_at_mm);
  }

  // ── PWM comp sweep (the duty=0 dead zone bug) ──
  p.target_velocity_mm_s = 100.0f;
  p.max_current_a = 2.5f;
  p.mu_static = 0.5f;
  p.mu_kinetic = 0.4f;
  printf("\n--- PWM comp sweep (3-step, v=100, I=2.5, mu_s=0.5, mu_k=0.4) ---\n");
  printf("  %-8s  %-10s  %-10s  %-8s\n", "comp", "result", "time_ms", "switches");
  for (float c : {0.0f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.5f}) {
    p.pwm_compensation = c;

    float path[3][2];
    for (int i = 0; i < 3; i++) { path[i][0] = (i+1) * GRID_TO_MM; path[i][1] = 0; }

    PieceState piece; piece.reset(0, 0);
    SimResult r = simulate(piece, path, 3, p);
    printf("  %5.2f     %-10s  %7.0f    %d\n",
           c, r.error == MoveError::NONE ? "OK" : "STUCK",
           r.elapsed_ms, r.coil_switches);
  }

  return 0;
}
