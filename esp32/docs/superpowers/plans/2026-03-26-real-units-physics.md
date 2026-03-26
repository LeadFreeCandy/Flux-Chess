# Real-Units Physics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the physics simulation to use mm/g/mN units with precomputed force table lookup, dynamic friction from Fz, and controlled stopping.

**Architecture:** Replace PhysicsParams and the simulation loop in physics.h. Force comes from table lookup instead of analytical model. Friction is dynamic: mu × (weight + Fz × current). Stopping is distance-based with optional active braking. All coordinates in mm. Frontend and API updated to match.

**Tech Stack:** C++ / Arduino on ESP32-S3, React/TypeScript frontend, Python codegen

---

### Task 1: Rewrite physics.h — new PhysicsParams, PieceState, and execute()

**Files:**
- Modify: `firmware/physics.h`

This is the core change. Read the spec at `docs/superpowers/specs/2026-03-26-real-units-physics-design.md` for full details.

- [ ] **Step 1: Replace PhysicsParams and PieceState**

Replace everything from `// Firmware constant` through end of `PhysicsParams` struct with:

```cpp
// Coordinate conversion: 1 grid unit = 38/3 mm = 12.667 mm
static constexpr float GRID_TO_MM = 38.0f / 3.0f;

struct PieceState {
  float x, y;     // mm
  float vx, vy;   // mm/s
  bool stuck;

  void reset(float px_mm, float py_mm) {
    x = px_mm; y = py_mm;
    vx = 0; vy = 0;
    stuck = true;
  }
};

struct PhysicsParams {
  float piece_mass_g       = 4.3f;
  float max_current_a      = 1.0f;
  float mu_static          = 0.35f;
  float mu_kinetic         = 0.25f;
  float target_velocity_mm_s = 100.0f;
  float target_accel_mm_s2   = 500.0f;
  bool  active_brake       = true;
  uint16_t max_duration_ms = 5000;
};
```

- [ ] **Step 2: Add bitToLayer helper to private section of PhysicsMove**

After `coordToBit`, add:

```cpp
  static constexpr int BIT_TO_LAYER[] = {2, 1, 0, 3, 4};

  static int bitToLayer(uint8_t global_bit) {
    uint8_t local = global_bit % 8;
    return (local < 5) ? BIT_TO_LAYER[local] : 0;
  }
```

- [ ] **Step 3: Remove old force model functions**

Delete `coilForce()`, `maxForce()`, and `forceToDuty()` from the private section. Keep `tableForceFx`, `tableForceFy`, `tableForceFz`, `coordToBit`.

- [ ] **Step 4: Rewrite execute() — the full simulation loop**

Replace the entire `execute()` method with:

```cpp
  MoveError execute(PieceState& piece, const float path_mm[][2], int path_len,
                    const PhysicsParams& params) {
    if (path_len < 1) return MoveError::COIL_FAILURE;

    int coil_idx = 0;
    float cx = path_mm[0][0], cy = path_mm[0][1];

    float dx = path_mm[path_len - 1][0] - piece.x;
    float dy = path_mm[path_len - 1][1] - piece.y;
    bool moveX = (fabsf(dx) > fabsf(dy));
    float move_sign = moveX ? (dx > 0 ? 1.0f : -1.0f) : (dy > 0 ? 1.0f : -1.0f);

    int8_t activeBit = coordToBit((uint8_t)(path_mm[0][0] / GRID_TO_MM + 0.5f),
                                   (uint8_t)(path_mm[0][1] / GRID_TO_MM + 0.5f));
    if (activeBit < 0) return MoveError::COIL_FAILURE;
    int activeLayer = bitToLayer(activeBit);

    float weight_mN = params.piece_mass_g * 9.81f;
    float mass_kg = params.piece_mass_g * 1e-3f;

    if (!hw_.startCoil((uint8_t)activeBit, 255)) return MoveError::COIL_FAILURE;

    unsigned long t0 = millis();
    unsigned long last_tick_us = micros();
    uint8_t last_duty = 255;
    float last_current = params.max_current_a;

    // Stats
    float max_speed = 0;
    int coil_switches = 0;
    bool braked = false;
    bool coasting = false;  // true after coil cut for stopping

    LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%.1f,%.1f) mm",
              path_len, piece.x, piece.y, path_mm[path_len-1][0], path_mm[path_len-1][1]);

    while (millis() - t0 < params.max_duration_ms) {
      // 1. dt
      unsigned long now_us = micros();
      float dt = (now_us - last_tick_us) / 1000000.0f;
      last_tick_us = now_us;
      if (dt <= 0 || dt > 0.1f) dt = 0.001f;

      // 2. Offset from active coil in mm
      float off_x = piece.x - cx;
      float off_y = piece.y - cy;

      // 3. Force table lookup (mN at 1A)
      float fx_1a = tableForceFx(activeLayer, off_x, off_y);
      float fy_1a = tableForceFy(activeLayer, off_x, off_y);
      float fz_1a = tableForceFz(activeLayer, off_x, off_y);

      float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      float v_along = (moveX ? piece.vx : piece.vy) * move_sign;

      // 4. Dynamic friction
      float normal_mN = weight_mN + fz_1a * last_current;
      if (normal_mN < 0) normal_mN = 0;
      float mu = (piece.stuck || speed < 0.1f) ? params.mu_static : params.mu_kinetic;
      float friction_mN = mu * normal_mN;

      // 5. Static friction check
      if (piece.stuck) {
        float avail = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a) * params.max_current_a;
        float static_fric = params.mu_static * (weight_mN + fz_1a * params.max_current_a);
        if (avail > fmaxf(static_fric, 0)) {
          piece.stuck = false;
          last_tick_us = micros();
          LOG_BOARD("physics: unstuck (F=%.1f > fric=%.1f mN)", avail, static_fric);
        } else {
          if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
            hw_.stopCoil((uint8_t)activeBit);
            return MoveError::COIL_FAILURE;
          }
          continue;
        }
      }

      float fx, fy;
      uint8_t duty;

      if (!coasting) {
        // 6. Stopping check
        float dest_x = path_mm[path_len-1][0];
        float dest_y = path_mm[path_len-1][1];
        float dist_remain = moveX ? fabsf(dest_x - piece.x) : fabsf(dest_y - piece.y);

        float friction_decel = (weight_mN > 0) ? params.mu_kinetic * weight_mN / mass_kg : 0;
        float brake_decel = friction_decel;

        if (params.active_brake && coil_idx > 0) {
          // Lookup brake force from previous coil
          int8_t prevBit = coordToBit((uint8_t)(path_mm[coil_idx-1][0] / GRID_TO_MM + 0.5f),
                                      (uint8_t)(path_mm[coil_idx-1][1] / GRID_TO_MM + 0.5f));
          if (prevBit >= 0) {
            int prevLayer = bitToLayer(prevBit);
            float prev_cx = path_mm[coil_idx-1][0];
            float prev_cy = path_mm[coil_idx-1][1];
            float bfx = tableForceFx(prevLayer, piece.x - prev_cx, piece.y - prev_cy);
            float bfy = tableForceFy(prevLayer, piece.x - prev_cx, piece.y - prev_cy);
            float brake_force = sqrtf(bfx*bfx + bfy*bfy) * params.max_current_a;
            brake_decel = friction_decel + brake_force / mass_kg;
            brake_decel = fminf(brake_decel, params.target_accel_mm_s2);
          }
        }

        float stopping_dist = (brake_decel > 0.01f) ? (v_along * v_along) / (2.0f * brake_decel) : 9999.0f;

        if (coil_idx == path_len - 1 && v_along > 0 && stopping_dist >= dist_remain) {
          // Time to stop
          coasting = true;
          hw_.stopCoil((uint8_t)activeBit);

          if (params.active_brake && coil_idx > 0) {
            int8_t prevBit = coordToBit((uint8_t)(path_mm[coil_idx-1][0] / GRID_TO_MM + 0.5f),
                                        (uint8_t)(path_mm[coil_idx-1][1] / GRID_TO_MM + 0.5f));
            if (prevBit >= 0) {
              hw_.startCoil((uint8_t)prevBit, 255);
              activeBit = prevBit;
              activeLayer = bitToLayer(prevBit);
              braked = true;
              LOG_BOARD("physics: braking at v=%.1f dist_remain=%.1f stop_dist=%.1f",
                        v_along, dist_remain, stopping_dist);
            }
          } else {
            LOG_BOARD("physics: coasting at v=%.1f dist_remain=%.1f", v_along, dist_remain);
          }
        }
      }

      if (coasting) {
        // Coasting/braking — friction + optional brake coil
        if (braked) {
          fx = tableForceFx(activeLayer, piece.x - cx, piece.y - cy) * params.max_current_a;
          fy = tableForceFy(activeLayer, piece.x - cx, piece.y - cy) * params.max_current_a;
          // cx/cy here is the brake coil, which pulls backwards — that's correct
        } else {
          fx = 0; fy = 0;
        }
        duty = braked ? 255 : 0;
      } else {
        // 7. Controller: desired force → current → duty
        float speed_error = params.target_velocity_mm_s - v_along;
        float desired_accel = fminf(fmaxf(speed_error / dt, -params.target_accel_mm_s2), params.target_accel_mm_s2);
        float desired_force = mass_kg * desired_accel + friction_mN;
        if (desired_force < 0) desired_force = 0;

        float avail_lateral = sqrtf(fx_1a * fx_1a + fy_1a * fy_1a);
        float required_current = (avail_lateral > 0.01f) ? desired_force / avail_lateral : params.max_current_a;
        required_current = fminf(required_current, params.max_current_a);
        if (required_current < 0) required_current = 0;

        duty = (uint8_t)(required_current / params.max_current_a * 255.0f);
        float actual_current = (duty / 255.0f) * params.max_current_a;
        last_current = actual_current;

        fx = fx_1a * actual_current;
        fy = fy_1a * actual_current;
      }

      // 8. Apply friction (always opposes velocity)
      if (speed > 0.1f) {
        float fric = params.mu_kinetic * fmaxf(normal_mN, 0);
        float max_fric_accel = speed / dt * mass_kg;
        if (fric > max_fric_accel) fric = max_fric_accel;
        fx -= fric * (piece.vx / speed);
        fy -= fric * (piece.vy / speed);
      }

      // 9. Update velocity: a = F/m
      piece.vx += (fx / mass_kg) * dt;
      piece.vy += (fy / mass_kg) * dt;

      speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
      if (speed > params.target_velocity_mm_s * 1.5f) {
        float scale = params.target_velocity_mm_s * 1.5f / speed;
        piece.vx *= scale; piece.vy *= scale;
      }
      if (speed > max_speed) max_speed = speed;

      // 10. Update position
      piece.x += piece.vx * dt;
      piece.y += piece.vy * dt;

      // Debug log every 100ms
      unsigned long elapsed = millis() - t0;
      if (elapsed / 100 != (elapsed - 1) / 100) {
        LOG_BOARD("physics: t=%lums pos=(%.1f,%.1f) v=(%.1f,%.1f) duty=%d %s",
                  elapsed, piece.x, piece.y, piece.vx, piece.vy, duty,
                  coasting ? (braked ? "BRAKE" : "COAST") : "");
      }

      // 11. Coil switching (only while not coasting)
      if (!coasting && coil_idx < path_len - 1) {
        bool passed = moveX
          ? ((dx > 0 && piece.x > cx + 0.1f) || (dx < 0 && piece.x < cx - 0.1f))
          : ((dy > 0 && piece.y > cy + 0.1f) || (dy < 0 && piece.y < cy - 0.1f));

        if (passed) {
          coil_idx++;
          cx = path_mm[coil_idx][0];
          cy = path_mm[coil_idx][1];

          uint8_t gx = (uint8_t)(cx / GRID_TO_MM + 0.5f);
          uint8_t gy = (uint8_t)(cy / GRID_TO_MM + 0.5f);
          int8_t newBit = coordToBit(gx, gy);
          if (newBit < 0) return MoveError::COIL_FAILURE;

          hw_.stopCoil((uint8_t)activeBit);
          activeBit = newBit;
          activeLayer = bitToLayer(newBit);
          hw_.startCoil((uint8_t)activeBit, 255);
          last_duty = 255;
          coil_switches++;

          LOG_BOARD("physics: switch coil %d at (%.1f,%.1f) L%d idx=%d/%d",
                    activeBit, cx, cy, activeLayer, coil_idx, path_len);
        }
      }

      // 12. PWM update
      if (!coasting && abs((int)duty - (int)last_duty) > 2) {
        hw_.sustainCoil((uint8_t)activeBit, 0, duty);
        last_duty = duty;
      }

      // 13. Arrival check
      float dest_x = path_mm[path_len-1][0];
      float dest_y = path_mm[path_len-1][1];
      float d_dest = sqrtf((piece.x-dest_x)*(piece.x-dest_x) + (piece.y-dest_y)*(piece.y-dest_y));

      if (d_dest < 1.0f && speed < 5.0f) {
        piece.x = dest_x; piece.y = dest_y;
        piece.vx = 0; piece.vy = 0;
        piece.stuck = true;

        if (!coasting) hw_.stopCoil((uint8_t)activeBit);
        if (braked) hw_.stopCoil((uint8_t)activeBit);

        unsigned long total_ms = millis() - t0;
        float dist_total = sqrtf(dx*dx + dy*dy);
        float avg_speed = (total_ms > 0) ? dist_total / (total_ms / 1000.0f) : 0;
        LOG_BOARD("physics: === MOVE COMPLETE ===");
        LOG_BOARD("physics: time=%lums dist=%.1fmm avg=%.0fmm/s max=%.0fmm/s switches=%d braked=%d",
                  total_ms, dist_total, avg_speed, max_speed, coil_switches, braked ? 1 : 0);
        return MoveError::NONE;
      }

      // Sustain
      if (!coasting) {
        if (!hw_.sustainCoil((uint8_t)activeBit, 100, last_duty)) {
          hw_.stopCoil((uint8_t)activeBit);
          return MoveError::COIL_FAILURE;
        }
      } else if (braked) {
        hw_.sustainCoil((uint8_t)activeBit, 100, 255);
      } else {
        delayMicroseconds(100);
      }
    }

    // Timeout
    if (!coasting) hw_.stopCoil((uint8_t)activeBit);
    if (braked) hw_.stopCoil((uint8_t)activeBit);
    LOG_BOARD("physics: TIMEOUT pos=(%.1f,%.1f) v=(%.1f,%.1f) max_speed=%.0f",
              piece.x, piece.y, piece.vx, piece.vy, max_speed);
    return MoveError::COIL_FAILURE;
  }
```

- [ ] **Step 5: Compile**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware
```

- [ ] **Step 6: Commit**

```bash
git add firmware/physics.h
git commit -m "feat: rewrite physics sim with real units (mm/g/mN), force tables, dynamic friction, stopping"
```

---

### Task 2: Update board.h — path in mm, PieceState in mm

**Files:**
- Modify: `firmware/board.h`

- [ ] **Step 1: Update movePhysicsOrthogonal to build path in mm**

Find `movePhysicsOrthogonal`. Change the path building to use mm and float:

Replace the path building section (from `// Build coil path` to `path_len++`) with:

```cpp
    // Build coil path in mm
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    float path_mm[GRID_COLS + GRID_ROWS][2];
    int path_len = 0;
    int8_t cx = fromX, cy = fromY;
    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;
      path_mm[path_len][0] = cx * GRID_TO_MM;
      path_mm[path_len][1] = cy * GRID_TO_MM;
      path_len++;
    }
```

Change `ps.reset(fromX, fromY)` to `ps.reset(fromX * GRID_TO_MM, fromY * GRID_TO_MM)`.

Change the `physics_.execute` call to pass `path_mm` instead of `path`.

- [ ] **Step 2: Update PieceState initialization in initDefaultBoard**

The `piece_states_` reset should use mm:

```cpp
    for (int x = 0; x < GRID_COLS; x++)
      for (int y = 0; y < GRID_ROWS; y++)
        piece_states_[x][y].reset(x * GRID_TO_MM, y * GRID_TO_MM);
```

- [ ] **Step 3: Compile**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware
```

- [ ] **Step 4: Commit**

```bash
git add firmware/board.h
git commit -m "feat: board path building in mm, PieceState positions in mm"
```

---

### Task 3: Update API, serial handler, codegen, and frontend

**Files:**
- Modify: `firmware/api.h`
- Modify: `firmware/serial_server.h`
- Run: `codegen/generate.py`
- Modify: `frontend/src/widgets/HexapawnWidget.tsx`
- Modify: `frontend/src/widgets/MoveTestWidget.tsx`

- [ ] **Step 1: Update MovePhysicsRequest in api.h**

Replace the existing `MovePhysicsRequest` with:

```cpp
struct MovePhysicsRequest {
  uint8_t from_x;
  uint8_t from_y;
  uint8_t to_x;
  uint8_t to_y;
  float piece_mass_g;
  float max_current_a;
  float mu_static;
  float mu_kinetic;
  float target_velocity_mm_s;
  float target_accel_mm_s2;
  uint16_t max_duration_ms;
};
```

(Note: `active_brake` bool is not sent from frontend — defaults to true on firmware)

- [ ] **Step 2: Update parsePhysicsParams in serial_server.h**

Replace the param parsing:

```cpp
inline PhysicsParams parsePhysicsParams(const String& params) {
  PhysicsParams p;
  String v;
  if ((v = jsonGet(params, "piece_mass_g")).length())       p.piece_mass_g = v.toFloat();
  if ((v = jsonGet(params, "max_current_a")).length())      p.max_current_a = v.toFloat();
  if ((v = jsonGet(params, "mu_static")).length())          p.mu_static = v.toFloat();
  if ((v = jsonGet(params, "mu_kinetic")).length())         p.mu_kinetic = v.toFloat();
  if ((v = jsonGet(params, "target_velocity_mm_s")).length()) p.target_velocity_mm_s = v.toFloat();
  if ((v = jsonGet(params, "target_accel_mm_s2")).length())  p.target_accel_mm_s2 = v.toFloat();
  if ((v = jsonGet(params, "max_duration_ms")).length())    p.max_duration_ms = v.toInt();
  return p;
}
```

- [ ] **Step 3: Run codegen**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && python3 codegen/generate.py
```

- [ ] **Step 4: Update HexapawnWidget.tsx**

Replace `DEFAULT_PHYSICS_PARAMS`, `PARAM_INFO`, and `PhysicsDebug`:

```tsx
const DEFAULT_PHYSICS_PARAMS = {
  piece_mass_g: 4.3,
  max_current_a: 1.0,
  mu_static: 0.35,
  mu_kinetic: 0.25,
  target_velocity_mm_s: 100,
  target_accel_mm_s2: 500,
  max_duration_ms: 5000,
};

const PARAM_INFO: Record<keyof typeof DEFAULT_PHYSICS_PARAMS, string> = {
  piece_mass_g:        "mass (g)",
  max_current_a:       "max current (A)",
  mu_static:           "static mu",
  mu_kinetic:          "kinetic mu",
  target_velocity_mm_s: "target v (mm/s)",
  target_accel_mm_s2:  "target a (mm/s²)",
  max_duration_ms:     "timeout (ms)",
};
```

Update `PhysicsDebug` to compute real-unit derived values:

```tsx
function PhysicsDebug({ p }: { p: typeof DEFAULT_PHYSICS_PARAMS }) {
  const { piece_mass_g, max_current_a, mu_static, mu_kinetic,
          target_velocity_mm_s: tv, target_accel_mm_s2: ta } = p;

  const weight = piece_mass_g * 9.81;  // mN
  const peak_fx = 54.0 * max_current_a;  // approx from force table L0
  const peak_fz = 101.0 * max_current_a;
  const normal_active = weight + peak_fz;
  const static_fric = mu_static * normal_active;
  const kinetic_fric = mu_kinetic * weight;  // coil off during coast
  const t_cross = 9 * 12.667 / tv;  // 9 grid units in mm
  const coast_decel = kinetic_fric / (piece_mass_g * 1e-3);
  const stop_dist = (tv * tv) / (2 * coast_decel);

  const row = (label: string, value: string) => (
    <div style={{ display: "flex", justifyContent: "space-between" }}>
      <span style={{ color: "#888" }}>{label}</span>
      <span style={{ color: "#e0e0e0" }}>{value}</span>
    </div>
  );

  return (
    <div style={{
      width: "100%", maxWidth: 400, marginBottom: 8,
      background: "#0a0a1a", padding: 10, borderRadius: 6, border: "1px solid #2a2a4a",
      fontSize: 11, fontFamily: "monospace", display: "flex", flexDirection: "column", gap: 2,
    }}>
      <div style={{ color: "#666", fontSize: 10, marginBottom: 4 }}>REAL-UNIT PHYSICS</div>
      {row("Piece weight", `${weight.toFixed(1)} mN`)}
      {row("Peak lateral (L0, 1A)", `${(54.0 * max_current_a).toFixed(1)} mN`)}
      {row("Peak Fz (L0, 1A)", `${peak_fz.toFixed(1)} mN (downward)`)}
      {row("Normal force (coil on)", `${normal_active.toFixed(1)} mN`)}
      {row("Static friction (coil on)", `${static_fric.toFixed(1)} mN`)}
      {row("Can overcome stiction?", peak_fx > static_fric ? `YES (${(peak_fx/static_fric).toFixed(1)}x)` : "NO")}
      {row("Coast friction (coil off)", `${kinetic_fric.toFixed(1)} mN`)}
      {row("Coast decel", `${coast_decel.toFixed(0)} mm/s²`)}
      {row("Stopping distance", `${stop_dist.toFixed(1)} mm at ${tv} mm/s`)}
      {row("Board traverse (114mm)", `${t_cross.toFixed(2)}s`)}
      {peak_fx < static_fric && (
        <div style={{ color: "#ef5350", marginTop: 4 }}>
          Peak force ({peak_fx.toFixed(1)}) &lt; static friction ({static_fric.toFixed(1)}) — piece won't move!
        </div>
      )}
    </div>
  );
}
```

Also update `updateParam` to use `parseFloat` (already does) and the `step` attribute for inputs.

- [ ] **Step 5: Update MoveTestWidget.tsx**

Replace `DEFAULT_PARAMS` and `PARAM_LABELS` to match the new params (same as HexapawnWidget defaults).

- [ ] **Step 6: Type-check frontend**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32/frontend && npx tsc --noEmit
```

- [ ] **Step 7: Compile firmware**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware
```

- [ ] **Step 8: Commit**

```bash
git add firmware/api.h firmware/serial_server.h frontend/src/widgets/HexapawnWidget.tsx frontend/src/widgets/MoveTestWidget.tsx
git commit -m "feat: update API and frontend for real-unit physics params"
```

---

### Task 4: Update host tests

**Files:**
- Modify: `tests/test_physics.cpp`

- [ ] **Step 1: Update PhysicsParams and simulation loop in test**

The test file has its own copy of the physics simulation. Update `PhysicsParams` to match the new struct. Update the simulation loop to use force table stubs (simple analytical approximation for testing — we don't need the actual tables on the host):

Replace `PhysicsParams` in the test with the new fields. For force lookup, use a simple stub:

```cpp
// Stub force model for host tests (approximates force table)
// Peak ~54 mN at 1A for layer 0 at ~6mm offset
float stubForceFx(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  if (d < 0.01f) return 0;
  float f = 54.0f * d / (d * d + 5.0f);  // peaks around d=2.2mm
  return -f * (dx_mm / d);  // toward center
}
float stubForceFy(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  if (d < 0.01f) return 0;
  float f = 54.0f * d / (d * d + 5.0f);
  return -f * (dy_mm / d);
}
float stubForceFz(float dx_mm, float dy_mm) {
  float d = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
  return -101.0f / (1.0f + d * d * 0.01f);  // strong at center, weaker at edges
}
```

Update the simulation loop to use mm, real friction, stopping logic.

- [ ] **Step 2: Run tests**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32/tests && g++ -std=c++17 -O2 -o test_physics test_physics.cpp && ./test_physics
```

- [ ] **Step 3: Commit**

```bash
git add tests/test_physics.cpp
git commit -m "feat: update host tests for real-unit physics"
```
