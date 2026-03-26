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
#define LOG_HW(fmt, ...) do { if (verbose) printf("[HW]    " fmt "\n", ##__VA_ARGS__); } while(0)

static bool verbose = false;

// Stub MoveError
enum class MoveError : uint8_t {
  NONE, OUT_OF_BOUNDS, SAME_POSITION, NOT_ORTHOGONAL,
  NO_PIECE_AT_SOURCE, PATH_BLOCKED, COIL_FAILURE
};

// Grid constants (from api.h)
constexpr uint8_t GRID_COLS = 10;
constexpr uint8_t GRID_ROWS = 7;
constexpr uint8_t NUM_HALL_SENSORS = 12;
constexpr uint8_t SENSOR_COLS = 4;
constexpr uint8_t SENSOR_ROWS = 3;

// ═══════════════════════════════════════════════════════════════
// Physics types (extracted from physics.h to avoid Arduino deps)
// ═══════════════════════════════════════════════════════════════

static constexpr float SENSOR_VELOCITY_WEIGHT = 0.3f;

struct PieceState {
  float x, y;
  float vx, vy;
  bool stuck;

  void reset(float px, float py) {
    x = px; y = py;
    vx = 0; vy = 0;
    stuck = true;
  }
};

struct PhysicsParams {
  float force_k          = 10.0f;
  float force_epsilon    = 0.3f;
  float falloff_exp      = 2.0f;
  float voltage_scale    = 1.0f;
  float friction_static  = 3.0f;
  float friction_kinetic = 2.0f;
  float target_velocity  = 5.0f;
  float target_accel     = 20.0f;
  float sensor_k         = 500.0f;
  float sensor_falloff   = 2.0f;
  float sensor_threshold = 50.0f;
  float manual_baseline  = 2030.0f;
  float manual_piece_mean = 1700.0f;
  uint16_t max_duration_ms = 5000;
};

struct CalSensorData {
  float baseline_mean;
  float piece_mean;
};

// ═══════════════════════════════════════════════════════════════
// Pure physics simulation (no hardware interaction)
// ═══════════════════════════════════════════════════════════════

static constexpr uint8_t SR_COLS = 4;
static constexpr uint8_t SR_ROWS_GRID = 3;
static constexpr uint8_t SR_BLOCK = 3;

static int8_t coordToBit(uint8_t x, uint8_t y) {
  uint8_t sr_col = x / SR_BLOCK;
  uint8_t sr_row = y / SR_BLOCK;
  if (sr_col >= SR_COLS || sr_row >= SR_ROWS_GRID) return -1;
  uint8_t sr_index = sr_col * SR_ROWS_GRID + sr_row;
  uint8_t lx = x % SR_BLOCK;
  uint8_t ly = y % SR_BLOCK;
  int8_t local_bit = -1;
  if (ly == 0) local_bit = 2 - lx;
  else if (lx == 0) local_bit = 2 + ly;
  if (local_bit < 0) return -1;
  return (int8_t)(sr_index * 8 + local_bit);
}

static uint8_t sensorAt(uint8_t x, uint8_t y) {
  return (x / SR_BLOCK) * SR_ROWS_GRID + (SR_ROWS_GRID - 1 - (y / SR_BLOCK));
}

static float coilForce(float d, const PhysicsParams& p) {
  return p.voltage_scale * p.force_k * d / (powf(d, p.falloff_exp) + p.force_epsilon);
}

static float maxForce(const PhysicsParams& p) {
  float d_peak = powf(p.force_epsilon, 1.0f / p.falloff_exp);
  return coilForce(d_peak, p);
}

static uint8_t forceToDuty(float force, const PhysicsParams& p) {
  float mf = maxForce(p);
  if (mf <= 0) return 255;
  float ratio = force / mf;
  if (ratio > 1.0f) ratio = 1.0f;
  if (ratio < 0.0f) ratio = 0.0f;
  return (uint8_t)(ratio * 255.0f);
}

static float sensorDistance(float strength, const PhysicsParams& p) {
  if (strength <= 0) return 99.0f;
  float ratio = p.sensor_k / strength;
  if (ratio <= p.force_epsilon) return 0.0f;
  return powf(ratio - p.force_epsilon, 1.0f / p.sensor_falloff);
}

// Simulated sensor reading based on piece distance from sensor center
static uint16_t simulateSensorReading(float piece_x, float piece_y,
                                       uint8_t sensor_x, uint8_t sensor_y,
                                       const CalSensorData& cal) {
  float dx = piece_x - sensor_x;
  float dy = piece_y - sensor_y;
  float dist = sqrtf(dx * dx + dy * dy);

  // Inverse of sensor model: reading drops as piece gets closer
  float delta = cal.baseline_mean - cal.piece_mean;  // max drop at d=0
  float drop = delta / (1.0f + dist * dist);  // simple falloff
  return (uint16_t)(cal.baseline_mean - drop);
}

struct SimResult {
  MoveError error;
  float final_x, final_y;
  float final_vx, final_vy;
  float elapsed_ms;
  int ticks;
  std::vector<std::pair<float,float>> trajectory;
};

static constexpr float MIN_TICK_S = 0.0005f;  // 500us minimum tick

SimResult simulatePhysicsMove(PieceState& piece, const uint8_t path[][2], int path_len,
                               const PhysicsParams& params, const CalSensorData* cal_sensors = nullptr) {
  SimResult result;
  result.error = MoveError::COIL_FAILURE;
  result.ticks = 0;

  if (path_len < 1) return result;

  int coil_idx = 0;
  float cx = path[0][0], cy = path[0][1];

  float dx = path[path_len - 1][0] - piece.x;
  float dy = path[path_len - 1][1] - piece.y;
  bool moveX = (fabsf(dx) > fabsf(dy));

  int8_t activeBit = coordToBit(path[0][0], path[0][1]);
  if (activeBit < 0) return result;

  float elapsed_s = 0;
  float max_s = params.max_duration_ms / 1000.0f;

  float prev_sensor_pos = 0;
  bool prev_sensor_valid = false;

  LOG_BOARD("sim: start path_len=%d from=(%.1f,%.1f) to=(%d,%d)",
            path_len, piece.x, piece.y, path[path_len-1][0], path[path_len-1][1]);

  while (elapsed_s < max_s) {
    float dt = MIN_TICK_S;
    elapsed_s += dt;
    result.ticks++;

    // 2. Compute distance and direction to active coil
    float ddx = cx - piece.x;
    float ddy = cy - piece.y;
    float dist = sqrtf(ddx * ddx + ddy * ddy);
    float dir_x = 0, dir_y = 0;
    if (dist > 0.001f) { dir_x = ddx / dist; dir_y = ddy / dist; }

    float f_available = coilForce(dist, params);

    // 3. Static friction check
    if (piece.stuck) {
      if (f_available > params.friction_static) {
        piece.stuck = false;
        LOG_BOARD("sim: static friction overcome (F=%.2f > %.2f) at t=%.1fms",
                  f_available, params.friction_static, elapsed_s * 1000);
      } else {
        continue;
      }
    }

    // 4. Desired force along movement axis
    float v_along = (moveX ? piece.vx : piece.vy) * (moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f));
    float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);

    float speed_error = params.target_velocity - v_along;
    float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel), params.target_accel);

    float friction_force = (speed > 0.001f) ? params.friction_kinetic * speed : 0;
    float desired_force = desired_accel + friction_force;
    if (desired_force < 0) desired_force = 0;

    // 5. Duty and actual force
    float duty_f = (f_available > 0.001f) ? (desired_force / f_available) : 1.0f;
    if (duty_f > 1.0f) duty_f = 1.0f;
    if (duty_f < 0.0f) duty_f = 0.0f;
    float actual_force = f_available * duty_f;

    float fx = actual_force * dir_x;
    float fy = actual_force * dir_y;

    // Friction
    if (speed > 0.001f) {
      float fric = params.friction_kinetic * speed;
      float max_fric = speed / dt;
      if (fric > max_fric) fric = max_fric;
      fx -= fric * (piece.vx / speed);
      fy -= fric * (piece.vy / speed);
    }

    // 6. Update velocity
    piece.vx += fx * dt;
    piece.vy += fy * dt;

    // Clamp velocity (safety)
    speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
    if (speed > params.target_velocity * 1.5f) {
      float scale = params.target_velocity * 1.5f / speed;
      piece.vx *= scale;
      piece.vy *= scale;
    }

    // 8. Update position
    piece.x += piece.vx * dt;
    piece.y += piece.vy * dt;

    // Record trajectory every 10 ticks
    if (result.ticks % 10 == 0) {
      result.trajectory.push_back({piece.x, piece.y});
    }

    // 9. Coil switching
    if (coil_idx < path_len - 1) {
      bool passed = moveX
        ? ((dx > 0 && piece.x > cx + 0.01f) || (dx < 0 && piece.x < cx - 0.01f))
        : ((dy > 0 && piece.y > cy + 0.01f) || (dy < 0 && piece.y < cy - 0.01f));

      if (passed) {
        coil_idx++;
        cx = path[coil_idx][0];
        cy = path[coil_idx][1];
        int8_t newBit = coordToBit((uint8_t)cx, (uint8_t)cy);
        if (newBit < 0) return result;
        activeBit = newBit;
        prev_sensor_valid = false;

        LOG_BOARD("sim: switch to coil at (%d,%d) idx=%d/%d t=%.1fms",
                  (int)cx, (int)cy, coil_idx, path_len, elapsed_s * 1000);
      }
    }

    // 11. Sensor correction — for whichever sensor block the active coil is in
    if (cal_sensors) {
      uint8_t si = sensorAt((uint8_t)cx, (uint8_t)cy);
      uint8_t sensor_x = ((int)(cx / SR_BLOCK)) * SR_BLOCK;
      uint8_t sensor_y = ((int)(cy / SR_BLOCK)) * SR_BLOCK;
      uint16_t reading = simulateSensorReading(piece.x, piece.y, sensor_x, sensor_y, cal_sensors[si]);
      float baseline = cal_sensors[si].baseline_mean;
      float strength = baseline - reading;

      if (strength > params.sensor_threshold) {
        float sensor_dist = sensorDistance(strength, params);
        float scx = sensor_x;
        float scy = sensor_y;
        float dest_x = path[path_len-1][0];
        float dest_y = path[path_len-1][1];

        float sensor_pos;
        if (moveX) {
          float sim_dist = fabsf(piece.x - scx);
          if (sensor_dist < sim_dist) {
            float dir = (piece.x >= scx) ? 1.0f : -1.0f;
            piece.x = scx + dir * sensor_dist;
          }
          sensor_pos = piece.x;
        } else {
          float sim_dist = fabsf(piece.y - scy);
          if (sensor_dist < sim_dist) {
            float dir = (piece.y >= scy) ? 1.0f : -1.0f;
            piece.y = scy + dir * sensor_dist;
          }
          sensor_pos = piece.y;
        }

        if (prev_sensor_valid && dt > 0) {
          float v_sensor = (sensor_pos - prev_sensor_pos) / dt;
          if (moveX) {
            piece.vx = piece.vx * (1.0f - SENSOR_VELOCITY_WEIGHT)
                     + v_sensor * SENSOR_VELOCITY_WEIGHT;
          } else {
            piece.vy = piece.vy * (1.0f - SENSOR_VELOCITY_WEIGHT)
                     + v_sensor * SENSOR_VELOCITY_WEIGHT;
          }
        }
        prev_sensor_pos = sensor_pos;
        prev_sensor_valid = true;
      }
    }

    // 12. Arrival check — on last coil, sensor shows piece centered
    if (coil_idx == path_len - 1 && cal_sensors) {
      uint8_t si = sensorAt(path[path_len-1][0], path[path_len-1][1]);
      uint8_t sensor_x = ((int)(path[path_len-1][0] / SR_BLOCK)) * SR_BLOCK;
      uint8_t sensor_y = ((int)(path[path_len-1][1] / SR_BLOCK)) * SR_BLOCK;
      uint16_t reading = simulateSensorReading(piece.x, piece.y, sensor_x, sensor_y, cal_sensors[si]);
      float piece_mean = cal_sensors[si].piece_mean;
      if (fabsf(reading - piece_mean) < params.sensor_threshold) {
        LOG_BOARD("sim: arrived at (%d,%d) t=%.1fms reading=%d",
                  path[path_len-1][0], path[path_len-1][1], elapsed_s * 1000, reading);
        piece.x = path[path_len-1][0];
        piece.y = path[path_len-1][1];
        piece.vx = 0;
        piece.vy = 0;
        piece.stuck = true;
        result.error = MoveError::NONE;
        break;
      }
    }
  }

  result.final_x = piece.x;
  result.final_y = piece.y;
  result.final_vx = piece.vx;
  result.final_vy = piece.vy;
  result.elapsed_ms = elapsed_s * 1000;
  return result;
}

// ═══════════════════════════════════════════════════════════════
// Tests
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

void test_default_sanity() {
  printf("\n=== Default Parameter Sanity ===\n");
  PhysicsParams p;

  float d_peak = powf(p.force_epsilon, 1.0f / p.falloff_exp);
  float f_peak = coilForce(d_peak, p);
  float f_at_1 = coilForce(1.0f, p);
  float t_cross = 9.0f / p.target_velocity;
  float t_accel = p.target_velocity / p.target_accel;
  float d_brake = (p.target_velocity * p.target_velocity) / (2.0f * p.friction_kinetic);

  printf("  Peak force:        %.2f at d=%.2f\n", f_peak, d_peak);
  printf("  Force at d=1:      %.2f\n", f_at_1);
  printf("  Static friction:   %.2f\n", p.friction_static);
  printf("  Kinetic friction:  %.2f (per unit velocity)\n", p.friction_kinetic);
  printf("  Board cross time:  %.2fs (9 units at v=%.1f)\n", t_cross, p.target_velocity);
  printf("  Accel time:        %.0fms (to v=%.1f at a=%.1f)\n", t_accel * 1000, p.target_velocity, p.target_accel);
  printf("  Brake distance:    %.2f grid units\n", d_brake);

  TEST("Peak force > static friction (piece can move)") {
    printf("\n    peak=%.2f > static=%.2f\n    ", f_peak, p.friction_static);
    EXPECT(f_peak > p.friction_static) PASS;
  } END_TEST

  TEST("Static friction > kinetic friction") {
    EXPECT(p.friction_static > p.friction_kinetic) PASS;
  } END_TEST

  TEST("Force at d=1 (adjacent coil) > static friction") {
    printf("\n    f_at_1=%.2f > static=%.2f\n    ", f_at_1, p.friction_static);
    EXPECT(f_at_1 > p.friction_static) PASS;
  } END_TEST

  TEST("Braking distance < 10 grid units (friction-only worst case)") {
    printf("\n    d_brake=%.2f (coils assist in practice)\n    ", d_brake);
    EXPECT(d_brake < 10.0f) PASS;
  } END_TEST

  TEST("Board cross time < 5s") {
    EXPECT(t_cross < 5.0f) PASS;
  } END_TEST
}

void test_force_model() {
  printf("\n=== Force Model ===\n");
  PhysicsParams p;

  TEST("Force is zero at d=0") {
    float f = coilForce(0, p);
    EXPECT(fabsf(f) < 0.001f) PASS;
  } END_TEST

  TEST("Force peaks at intermediate distance") {
    float f_01 = coilForce(0.1f, p);
    float f_peak = coilForce(powf(p.force_epsilon, 1.0f / p.falloff_exp), p);
    float f_5 = coilForce(5.0f, p);
    EXPECT(f_peak > f_01 && f_peak > f_5) PASS;
  } END_TEST

  TEST("Force falls off at large distance") {
    float f_1 = coilForce(1.0f, p);
    float f_3 = coilForce(3.0f, p);
    float f_10 = coilForce(10.0f, p);
    EXPECT(f_1 > f_3 && f_3 > f_10) PASS;
  } END_TEST

  TEST("Force scales with voltage_scale") {
    PhysicsParams p2 = p;
    p2.voltage_scale = 2.0f;
    float f1 = coilForce(1.0f, p);
    float f2 = coilForce(1.0f, p2);
    EXPECT(fabsf(f2 - 2.0f * f1) < 0.001f) PASS;
  } END_TEST

  TEST("Duty is 0 at d=0 (zero force)") {
    uint8_t d = forceToDuty(coilForce(0, p), p);
    EXPECT(d == 0) PASS;
  } END_TEST

  TEST("Duty peaks at peak force distance") {
    float d_peak = powf(p.force_epsilon, 1.0f / p.falloff_exp);
    uint8_t d = forceToDuty(coilForce(d_peak, p), p);
    EXPECT(d == 255) PASS;
  } END_TEST
}

void test_friction_clamping() {
  printf("\n=== Friction Clamping ===\n");

  TEST("Piece with velocity doesn't reverse due to friction") {
    PhysicsParams p;
    p.friction_kinetic = 100.0f;  // absurdly high friction

    PieceState piece;
    piece.reset(0, 0);
    piece.stuck = false;
    piece.vy = 1.0f;  // moving up

    // One tick: high friction should stop, not reverse
    float dt = MIN_TICK_S;
    float speed = piece.vy;
    float fric = p.friction_kinetic * speed;
    float max_fric = speed / dt;
    if (fric > max_fric) fric = max_fric;
    piece.vy += (-fric) * dt;  // friction opposes motion

    EXPECT(piece.vy >= -0.001f) PASS;  // should not go negative
  } END_TEST
}

void test_simple_move_no_sensor() {
  printf("\n=== Simple Move (no sensor) ===\n");

  TEST("Piece moves from (0,0) toward (0,3) without sensors") {
    PhysicsParams p;
    p.max_duration_ms = 2000;

    PieceState piece;
    piece.reset(0, 0);

    // Path: (0,1), (0,2), (0,3)
    uint8_t path[3][2] = {{0,1}, {0,2}, {0,3}};

    SimResult r = simulatePhysicsMove(piece, path, 3, p, nullptr);

    printf("\n    final=(%.2f,%.2f) v=(%.2f,%.2f) t=%.0fms ticks=%d\n    ",
           r.final_x, r.final_y, r.final_vx, r.final_vy, r.elapsed_ms, r.ticks);

    // Without sensors, no arrival detection — should timeout
    // But piece should have moved in +Y direction
    EXPECT(r.final_y > 0.5f) PASS;
  } END_TEST
}

void test_simple_move_with_sensor() {
  printf("\n=== Simple Move (with sensor) ===\n");

  TEST("Piece moves from (0,0) to (0,3) with sensor feedback") {
    PhysicsParams p;
    p.max_duration_ms = 5000;

    PieceState piece;
    piece.reset(0, 0);

    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }

    uint8_t path[3][2] = {{0,1}, {0,2}, {0,3}};

    SimResult r = simulatePhysicsMove(piece, path, 3, p, cal);

    printf("\n    final=(%.2f,%.2f) v=(%.2f,%.2f) t=%.0fms ticks=%d err=%d\n    ",
           r.final_x, r.final_y, r.final_vx, r.final_vy, r.elapsed_ms, r.ticks, (int)r.error);

    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST

  TEST("Piece arrives near destination") {
    PhysicsParams p;
    PieceState piece;
    piece.reset(0, 0);

    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }

    uint8_t path[3][2] = {{0,1}, {0,2}, {0,3}};

    SimResult r = simulatePhysicsMove(piece, path, 3, p, cal);

    printf("\n    final=(%.2f,%.2f)\n    ", r.final_x, r.final_y);
    EXPECT(r.error == MoveError::NONE && fabsf(r.final_y - 3.0f) < 0.1f) PASS;
  } END_TEST
}

void test_horizontal_move() {
  printf("\n=== Horizontal Move ===\n");

  TEST("Piece moves from (0,0) to (3,0)") {
    PhysicsParams p;
    PieceState piece;
    piece.reset(0, 0);

    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }

    uint8_t path[3][2] = {{1,0}, {2,0}, {3,0}};

    SimResult r = simulatePhysicsMove(piece, path, 3, p, cal);

    printf("\n    final=(%.2f,%.2f) t=%.0fms err=%d\n    ",
           r.final_x, r.final_y, r.elapsed_ms, (int)r.error);
    EXPECT(r.error == MoveError::NONE && fabsf(r.final_x - 3.0f) < 0.1f) PASS;
  } END_TEST
}

void test_coil_switching() {
  printf("\n=== Coil Switching ===\n");

  TEST("Piece passes through intermediate coils") {
    PhysicsParams p;
    p.max_duration_ms = 3000;
    verbose = true;

    PieceState piece;
    piece.reset(0, 0);

    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }

    uint8_t path[3][2] = {{0,1}, {0,2}, {0,3}};

    SimResult r = simulatePhysicsMove(piece, path, 3, p, cal);

    printf("    final=(%.2f,%.2f) t=%.0fms switches detected in logs above\n    ",
           r.final_x, r.final_y, r.elapsed_ms);
    verbose = false;
    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST
}

void test_velocity_never_reverses() {
  printf("\n=== Velocity Stability ===\n");

  TEST("Y velocity stays non-negative for +Y move") {
    PhysicsParams p;
    p.friction_kinetic = 10.0f;  // high friction

    PieceState piece;
    piece.reset(0, 0);

    uint8_t path[1][2] = {{0,1}};

    // Run manually tracking min vy
    piece.stuck = false;  // skip static friction
    float min_vy = 999;
    float elapsed = 0;

    for (int i = 0; i < 10000 && elapsed < 2.0f; i++) {
      float dt = MIN_TICK_S;
      elapsed += dt;

      float ddy = 1.0f - piece.y;
      float dist = fabsf(ddy);
      float force_mag = coilForce(dist, p);
      float fy = (ddy > 0) ? force_mag : -force_mag;

      float speed = fabsf(piece.vy);
      if (speed > 0.001f) {
        float fric = p.friction_kinetic * speed;
        float max_fric = speed / dt;
        if (fric > max_fric) fric = max_fric;
        fy -= fric * (piece.vy > 0 ? 1.0f : -1.0f);
      }

      if (fabsf(fy) > p.target_accel) fy = (fy > 0 ? 1 : -1) * p.target_accel;
      piece.vy += fy * dt;
      if (fabsf(piece.vy) > p.target_velocity) piece.vy = (piece.vy > 0 ? 1 : -1) * p.target_velocity;
      piece.y += piece.vy * dt;

      if (piece.vy < min_vy) min_vy = piece.vy;
    }

    printf("\n    min_vy=%.4f final_y=%.2f\n    ", min_vy, piece.y);
    EXPECT(min_vy >= -0.01f) PASS;
  } END_TEST
}

void test_param_sensitivity() {
  printf("\n=== Parameter Sensitivity ===\n");

  auto run_move = [](PhysicsParams& p) -> SimResult {
    PieceState piece;
    piece.reset(0, 0);
    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }
    uint8_t path[3][2] = {{0,1}, {0,2}, {0,3}};
    return simulatePhysicsMove(piece, path, 3, p, cal);
  };

  TEST("Higher force_k moves piece faster") {
    PhysicsParams p1, p2;
    p1.force_k = 2.0f;
    p2.force_k = 10.0f;
    SimResult r1 = run_move(p1);
    SimResult r2 = run_move(p2);
    printf("\n    k=2: %.0fms, k=10: %.0fms\n    ", r1.elapsed_ms, r2.elapsed_ms);
    EXPECT(r2.elapsed_ms < r1.elapsed_ms || (r1.error != MoveError::NONE && r2.error == MoveError::NONE)) PASS;
  } END_TEST

  TEST("Higher friction slows piece down") {
    PhysicsParams p1, p2;
    p1.friction_kinetic = 1.0f;
    p2.friction_kinetic = 20.0f;
    SimResult r1 = run_move(p1);
    SimResult r2 = run_move(p2);
    printf("\n    fric=1: %.0fms, fric=20: %.0fms\n    ", r1.elapsed_ms, r2.elapsed_ms);
    EXPECT(r2.elapsed_ms > r1.elapsed_ms || r2.error != MoveError::NONE) PASS;
  } END_TEST

  TEST("Piece can't move if friction_static > peak force") {
    PhysicsParams p;
    p.friction_static = 999.0f;
    SimResult r = run_move(p);
    printf("\n    err=%d final_y=%.2f\n    ", (int)r.error, r.final_y);
    // Should timeout without moving
    EXPECT(r.error == MoveError::COIL_FAILURE && fabsf(r.final_y) < 0.1f) PASS;
  } END_TEST
}

void test_velocity_profile() {
  printf("\n=== Velocity Profile ===\n");

  TEST("Piece accelerates to target velocity correctly") {
    PhysicsParams p;
    PieceState piece;
    piece.reset(0, 0);
    piece.stuck = false; // skip static friction for this test

    // Single coil at (0,1), moving +Y
    float cx = 1.0f;
    float elapsed_s = 0;
    float dt = MIN_TICK_S;

    printf("\n    time(ms)  pos     vel     f_avail desired duty_f  actual\n");

    for (int i = 0; i < 2000 && elapsed_s < 1.0f; i++) {
      elapsed_s += dt;

      float dist = fabsf(cx - piece.y);
      float f_available = coilForce(dist, p);

      float v_along = piece.vy; // moving in +y
      float speed = fabsf(piece.vy);
      float speed_error = p.target_velocity - v_along;
      float desired_accel = fminf(fmaxf(speed_error / dt, -p.target_accel), p.target_accel);
      float friction_force = (speed > 0.001f) ? p.friction_kinetic * speed : 0;
      float desired_force = desired_accel + friction_force;
      if (desired_force < 0) desired_force = 0;

      float duty_f = (f_available > 0.001f) ? (desired_force / f_available) : 1.0f;
      if (duty_f > 1.0f) duty_f = 1.0f;
      float actual_force = f_available * duty_f;

      float fy = actual_force; // toward +y
      if (speed > 0.001f) {
        float fric = p.friction_kinetic * speed;
        float max_fric = speed / dt;
        if (fric > max_fric) fric = max_fric;
        fy -= fric;
      }

      piece.vy += fy * dt;
      if (piece.vy > p.target_velocity * 1.5f) piece.vy = p.target_velocity * 1.5f;
      piece.y += piece.vy * dt;

      if (i % 100 == 0) {
        printf("    %6.1f   %5.2f   %5.2f   %5.2f   %5.2f   %4.2f    %5.2f\n",
               elapsed_s * 1000, piece.y, piece.vy, f_available, desired_force, duty_f, actual_force);
      }
    }

    printf("    Final: pos=%.2f vel=%.2f at t=%.0fms\n", piece.y, piece.vy, elapsed_s * 1000);
    printf("    Note: duty=1.0 throughout means coil is at max — target_accel exceeds coil capability\n");
    printf("    Max achievable accel ≈ peak_force = %.1f gu/s² (target_accel=%.1f)\n    ",
           coilForce(powf(p.force_epsilon, 1.0f / p.falloff_exp), p), p.target_accel);

    // Piece should be accelerating (positive velocity)
    EXPECT(piece.vy > 2.0f) PASS;
  } END_TEST
}

void test_long_move() {
  printf("\n=== Long Move ===\n");

  TEST("Piece moves from (0,0) to (0,6) — full board height") {
    PhysicsParams p;
    p.max_duration_ms = 10000;

    PieceState piece;
    piece.reset(0, 0);

    CalSensorData cal[NUM_HALL_SENSORS];
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal[i].baseline_mean = 2030.0f;
      cal[i].piece_mean = 1700.0f;
    }

    uint8_t path[6][2] = {{0,1}, {0,2}, {0,3}, {0,4}, {0,5}, {0,6}};

    SimResult r = simulatePhysicsMove(piece, path, 6, p, cal);

    printf("\n    final=(%.2f,%.2f) t=%.0fms err=%d\n    ",
           r.final_x, r.final_y, r.elapsed_ms, (int)r.error);
    EXPECT(r.error == MoveError::NONE) PASS;
  } END_TEST
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "-v") == 0) verbose = true;

  printf("FluxChess Physics Simulation Tests\n");
  printf("===================================\n");

  test_default_sanity();
  test_force_model();
  test_friction_clamping();
  test_simple_move_no_sensor();
  test_simple_move_with_sensor();
  test_horizontal_move();
  test_coil_switching();
  test_velocity_never_reverses();
  test_param_sensitivity();
  test_velocity_profile();
  test_long_move();

  printf("\n===================================\n");
  printf("Results: %d passed, %d failed\n\n", tests_passed, tests_failed);

  return tests_failed > 0 ? 1 : 0;
}
