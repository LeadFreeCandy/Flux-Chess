# Physics-Based Piece Movement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the smart move system with a physics simulation that models coil force, friction, and sensor feedback to move pieces with sub-millisecond control.

**Architecture:** New `physics.h` file contains the simulation loop and data types. Board delegates to it. Hardware gets one new method (`sustainCoil`). Old smart move code is removed entirely. API and frontend updated to pass physics params.

**Tech Stack:** C++ / Arduino on ESP32-S3, React/TypeScript frontend, Python codegen

---

### Task 1: Add `sustainCoil` to Hardware

**Files:**
- Modify: `firmware/hardware.h`

- [ ] **Step 1: Add `sustainCoil` method to public API section (after `pulseBit`)**

```cpp
// Sustain the currently active coil without SPI writes.
// Must be the same bit as the last pulseBit/sustainCoil call.
// Returns false if bit mismatch or validation fails.
bool sustainCoil(uint8_t globalBit, uint16_t duration_us, uint8_t pwm_duty = 255) {
  if (!validateBit(globalBit)) return false;

  // Verify this bit is currently active
  uint8_t reg = globalBit / 8;
  uint8_t pos = globalBit % 8;
  if (!(sr_state_[reg] & (1 << pos))) {
    LOG_HW("sustainCoil REJECT: bit %d not active", globalBit);
    return false;
  }

  srSetPWM(pwm_duty);
  delayMicroseconds(duration_us);

  return true;
}
```

- [ ] **Step 2: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 3: Commit**

```bash
git add firmware/hardware.h
git commit -m "feat: add sustainCoil to Hardware for sub-ms coil control"
```

---

### Task 2: Create `physics.h` with data types

**Files:**
- Create: `firmware/physics.h`

- [ ] **Step 1: Create `physics.h` with `PieceState`, `PhysicsParams`, and the `PhysicsMove` class skeleton**

```cpp
#pragma once
#include <Arduino.h>
#include <math.h>
#include "hardware.h"
#include "utils.h"
#include "api.h"

// Firmware constant — not tunable from frontend
static constexpr float SENSOR_VELOCITY_WEIGHT = 0.3f;

struct PieceState {
  float x, y;     // estimated position in grid coords
  float vx, vy;   // velocity in grid-units/sec
  bool stuck;      // static friction not yet overcome

  void reset(float px, float py) {
    x = px; y = py;
    vx = 0; vy = 0;
    stuck = true;
  }
};

struct PhysicsParams {
  // Force model: F = voltage_scale * force_k / (d^falloff_exp + epsilon)
  float force_k          = 5.0f;
  float force_epsilon    = 0.5f;
  float falloff_exp      = 2.0f;
  float voltage_scale    = 1.0f;

  // Friction
  float friction_static  = 1.0f;
  float friction_kinetic = 3.0f;

  // Control targets
  float target_velocity  = 3.0f;   // grid-units/sec
  float target_accel     = 10.0f;  // grid-units/sec^2

  // Sensor distance model (same shape as force, inverted)
  float sensor_k         = 500.0f;
  float sensor_falloff   = 2.0f;
  float sensor_threshold = 50.0f;  // min (baseline - reading) to activate

  // Safety
  uint16_t max_duration_ms = 5000;
};

// Forward declaration — Board provides calibration data
struct CalSensorData {
  float baseline_mean;
  float piece_mean;
};

class PhysicsMove {
public:
  PhysicsMove(Hardware& hw) : hw_(hw) {}

  // Set calibration data pointer (called by Board when cal data is available)
  void setCalData(const CalSensorData* sensors, int count) {
    cal_sensors_ = sensors;
    cal_count_ = count;
  }

  // Execute a physics-based move along a coil path.
  // path[] = array of {x,y} coil positions, path_len = number of coils.
  // Modifies piece state in-place. Returns MoveError.
  MoveError execute(PieceState& piece, const uint8_t path[][2], int path_len,
                    const PhysicsParams& params);

private:
  Hardware& hw_;
  const CalSensorData* cal_sensors_ = nullptr;
  int cal_count_ = 0;

  // Grid mapping (duplicated from Board — these are pure functions)
  static constexpr uint8_t SR_COLS = 4;
  static constexpr uint8_t SR_ROWS = 3;
  static constexpr uint8_t SR_BLOCK = 3;

  static int8_t coordToBit(uint8_t x, uint8_t y) {
    uint8_t sr_col = x / SR_BLOCK;
    uint8_t sr_row = y / SR_BLOCK;
    if (sr_col >= SR_COLS || sr_row >= SR_ROWS) return -1;
    uint8_t sr_index = sr_col * SR_ROWS + sr_row;
    uint8_t lx = x % SR_BLOCK;
    uint8_t ly = y % SR_BLOCK;
    int8_t local_bit = -1;
    if (ly == 0) local_bit = 2 - lx;
    else if (lx == 0) local_bit = 2 + ly;
    if (local_bit < 0) return -1;
    return (int8_t)(sr_index * 8 + local_bit);
  }

  // Get sensor index for grid position
  static uint8_t sensorAt(uint8_t x, uint8_t y) {
    return (x / SR_BLOCK) * SR_ROWS + (SR_ROWS - 1 - (y / SR_BLOCK));
  }

  // Compute coil force magnitude at distance d
  float coilForce(float d, const PhysicsParams& p) {
    return p.voltage_scale * p.force_k / (powf(d, p.falloff_exp) + p.force_epsilon);
  }

  // Max possible force (at d=0) for PWM scaling
  float maxForce(const PhysicsParams& p) {
    return p.voltage_scale * p.force_k / p.force_epsilon;
  }

  // Estimate distance from sensor center given a reading
  float sensorDistance(float strength, const PhysicsParams& p) {
    if (strength <= 0) return 99.0f;  // no signal
    float ratio = p.sensor_k / strength;
    if (ratio <= p.force_epsilon) return 0.0f;
    return powf(ratio - p.force_epsilon, 1.0f / p.sensor_falloff);
  }

  // Force to PWM duty mapping
  uint8_t forceToDuty(float force, const PhysicsParams& p) {
    float mf = maxForce(p);
    if (mf <= 0) return 255;
    float ratio = force / mf;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    return (uint8_t)(ratio * 255.0f);
  }
};
```

- [ ] **Step 2: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles (physics.h not yet included anywhere, but syntax should be valid)

- [ ] **Step 3: Commit**

```bash
git add firmware/physics.h
git commit -m "feat: add physics.h with PieceState, PhysicsParams, PhysicsMove skeleton"
```

---

### Task 3: Implement the simulation loop

**Files:**
- Modify: `firmware/physics.h`

- [ ] **Step 1: Implement `PhysicsMove::execute` at the bottom of `physics.h`**

```cpp
inline MoveError PhysicsMove::execute(
    PieceState& piece, const uint8_t path[][2], int path_len,
    const PhysicsParams& params)
{
  if (path_len < 1) return MoveError::COIL_FAILURE;

  int coil_idx = 0;  // current target coil in path
  float cx = path[0][0], cy = path[0][1];  // active coil position

  // Determine movement direction
  float dx = path[path_len - 1][0] - piece.x;
  float dy = path[path_len - 1][1] - piece.y;
  bool moveX = (fabsf(dx) > fabsf(dy));  // primary axis

  // Activate first coil
  int8_t activeBit = coordToBit(path[0][0], path[0][1]);
  if (activeBit < 0) return MoveError::COIL_FAILURE;

  float force_mag = coilForce(0, params);
  uint8_t duty = forceToDuty(force_mag, params);
  if (!hw_.pulseBit((uint8_t)activeBit, 1, duty)) return MoveError::COIL_FAILURE;

  unsigned long t0 = millis();
  unsigned long last_tick_us = micros();

  // Sensor correction state
  float prev_sensor_pos = 0;
  bool prev_sensor_valid = false;

  uint8_t last_duty = duty;

  LOG_BOARD("physics: start path_len=%d from=(%.1f,%.1f) to=(%d,%d)",
            path_len, piece.x, piece.y, path[path_len-1][0], path[path_len-1][1]);

  while (millis() - t0 < params.max_duration_ms) {
    // 1. Measure dt
    unsigned long now_us = micros();
    float dt = (now_us - last_tick_us) / 1000000.0f;
    last_tick_us = now_us;
    if (dt <= 0 || dt > 0.1f) dt = 0.001f;  // sanity clamp

    // 2. Compute distance to active coil and force
    float ddx = cx - piece.x;
    float ddy = cy - piece.y;
    float dist = sqrtf(ddx * ddx + ddy * ddy);
    force_mag = coilForce(dist, params);

    // Force direction (unit vector toward coil)
    float fx = 0, fy = 0;
    if (dist > 0.001f) {
      fx = force_mag * (ddx / dist);
      fy = force_mag * (ddy / dist);
    }

    // 3. Friction
    if (piece.stuck) {
      if (force_mag > params.friction_static) {
        piece.stuck = false;
        LOG_BOARD("physics: static friction overcome (F=%.2f > %.2f)", force_mag, params.friction_static);
      } else {
        // Still stuck — just sustain coil and continue
        hw_.sustainCoil((uint8_t)activeBit, 100, last_duty);
        continue;
      }
    }

    float speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
    if (speed > 0.001f) {
      float fric = params.friction_kinetic * speed;
      fx -= fric * (piece.vx / speed);
      fy -= fric * (piece.vy / speed);
    }

    // 4-5. Net force, clamp acceleration
    float accel = sqrtf(fx * fx + fy * fy);
    if (accel > params.target_accel) {
      float scale = params.target_accel / accel;
      fx *= scale;
      fy *= scale;
    }

    // 6. Update velocity
    piece.vx += fx * dt;
    piece.vy += fy * dt;

    // 7. Clamp velocity
    speed = sqrtf(piece.vx * piece.vx + piece.vy * piece.vy);
    if (speed > params.target_velocity) {
      float scale = params.target_velocity / speed;
      piece.vx *= scale;
      piece.vy *= scale;
    }

    // 8. Update position
    piece.x += piece.vx * dt;
    piece.y += piece.vy * dt;

    // 9. Coil switching — check if piece passed active coil center
    if (coil_idx < path_len - 1) {
      bool passed = moveX
        ? ((dx > 0 && piece.x > cx + 0.01f) || (dx < 0 && piece.x < cx - 0.01f))
        : ((dy > 0 && piece.y > cy + 0.01f) || (dy < 0 && piece.y < cy - 0.01f));

      if (passed) {
        coil_idx++;
        cx = path[coil_idx][0];
        cy = path[coil_idx][1];
        int8_t newBit = coordToBit((uint8_t)cx, (uint8_t)cy);
        if (newBit < 0) return MoveError::COIL_FAILURE;

        // Switch coil via full pulseBit (SPI write)
        activeBit = newBit;
        uint8_t newDuty = forceToDuty(coilForce(0, params), params);
        hw_.pulseBit((uint8_t)activeBit, 1, newDuty);
        last_duty = newDuty;
        prev_sensor_valid = false;  // reset sensor state for new block

        LOG_BOARD("physics: switch to coil %d at (%d,%d) idx=%d/%d",
                  activeBit, (int)cx, (int)cy, coil_idx, path_len);
      }
    }

    // 10. Set PWM
    duty = forceToDuty(force_mag, params);
    if (abs((int)duty - (int)last_duty) > 2) {
      last_duty = duty;
    }

    // 11. Sensor correction
    if (cal_sensors_) {
      uint8_t si = sensorAt((uint8_t)cx, (uint8_t)cy);
      if (si < cal_count_) {
        uint16_t reading = hw_.readSensor(si);
        float baseline = cal_sensors_[si].baseline_mean;
        float strength = baseline - reading;

        if (strength > params.sensor_threshold) {
          float sensor_dist = sensorDistance(strength, params);
          // Sensor center position
          float scx = ((int)(cx / SR_BLOCK)) * SR_BLOCK;
          float scy = ((int)(cy / SR_BLOCK)) * SR_BLOCK;

          // Apply correction along movement axis only
          float sensor_pos;
          if (moveX) {
            float dir = (dx > 0) ? -1.0f : 1.0f;
            piece.x = scx + dir * sensor_dist;
            sensor_pos = piece.x;
          } else {
            float dir = (dy > 0) ? -1.0f : 1.0f;
            piece.y = scy + dir * sensor_dist;
            sensor_pos = piece.y;
          }

          // Velocity correction from successive sensor readings
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
    }

    // 12. Arrival check — on last coil, sensor shows piece centered
    if (coil_idx == path_len - 1 && cal_sensors_) {
      uint8_t si = sensorAt(path[path_len-1][0], path[path_len-1][1]);
      if (si < cal_count_) {
        uint16_t reading = hw_.readSensor(si);
        float piece_mean = cal_sensors_[si].piece_mean;
        if (fabsf(reading - piece_mean) < params.sensor_threshold) {
          LOG_BOARD("physics: arrived at (%d,%d) reading=%d piece_mean=%.0f",
                    path[path_len-1][0], path[path_len-1][1], reading, piece_mean);

          // Optional brake if still moving forward
          if (coil_idx > 0) {
            float v_along = moveX ? piece.vx : piece.vy;
            float move_dir = moveX ? dx : dy;
            if (v_along * move_dir > 0.1f) {
              int8_t prevBit = coordToBit(path[coil_idx-1][0], path[coil_idx-1][1]);
              if (prevBit >= 0) {
                hw_.pulseBit((uint8_t)prevBit, 30, 180);
                LOG_BOARD("physics: brake applied");
              }
            }
          }

          // Final position snap
          piece.x = path[path_len-1][0];
          piece.y = path[path_len-1][1];
          piece.vx = 0;
          piece.vy = 0;
          piece.stuck = true;

          // Turn off coil
          hw_.pulseBit((uint8_t)activeBit, 1, 0);
          return MoveError::NONE;
        }
      }
    }

    // Sustain coil for next tick
    hw_.sustainCoil((uint8_t)activeBit, 100, last_duty);
  }

  // Timeout
  hw_.pulseBit((uint8_t)activeBit, 1, 0);  // turn off
  LOG_BOARD("physics: TIMEOUT after %dms pos=(%.1f,%.1f)", params.max_duration_ms, piece.x, piece.y);
  return MoveError::COIL_FAILURE;
}
```

- [ ] **Step 2: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles (physics.h not yet included in board.h)

- [ ] **Step 3: Commit**

```bash
git add firmware/physics.h
git commit -m "feat: implement PhysicsMove simulation loop"
```

---

### Task 4: Remove old smart move code

**Files:**
- Modify: `firmware/board.h`
- Modify: `firmware/hardware.h`
- Modify: `firmware/api.h`
- Modify: `firmware/serial_server.h`

- [ ] **Step 1: In `board.h`, remove `SmartParams` struct (lines 21-29), `moveSmartOrthogonal` method (lines 116-160), `smartHop` method (lines 416-456), `sensorAt` helper (lines 407-408), `sensorThreshold` helper (lines 410-413), and the `SMART_*` constants reference in the comment at top. Update the header comment to remove `pulseUntilSensor` reference.**

The header comment should become:
```cpp
// Board is the game-logic layer. It must NOT directly manipulate shift registers,
// OE, or PWM. All coil actuation goes through Hardware's safe public API:
//   pulseBit()      — fixed-duration pulse with thermal protection
//   sustainCoil()   — sustain active coil without SPI writes
// Hardware encapsulates all dangerous SR/OE/PWM operations as private.
```

- [ ] **Step 2: In `hardware.h`, remove the `pulseUntilSensor` method and `PulseResult` struct (lines 103-152)**

- [ ] **Step 3: In `api.h`, remove `MoveSmartRequest` struct (lines 131-143) and the `move_smart` API_COMMAND line (line 159)**

- [ ] **Step 4: In `serial_server.h`, remove `handleMoveSmart` function and the `on("move_smart", ...)` registration**

- [ ] **Step 5: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 6: Commit**

```bash
git add firmware/board.h firmware/hardware.h firmware/api.h firmware/serial_server.h
git commit -m "refactor: remove smart move code (SmartParams, smartHop, pulseUntilSensor)"
```

---

### Task 5: Wire `PhysicsMove` into Board

**Files:**
- Modify: `firmware/board.h`

- [ ] **Step 1: Add `#include "physics.h"` at the top, add `PieceState` and `PhysicsMove` members, and add `movePhysicsOrthogonal` method**

After `#include "hardware.h"`, add:
```cpp
#include "physics.h"
```

Add to `Board` private members:
```cpp
  PieceState piece_states_[GRID_COLS][GRID_ROWS];
  PhysicsMove physics_{hw_};
```

In `initDefaultBoard()`, after setting pieces, add:
```cpp
    // Reset physics states
    for (int x = 0; x < GRID_COLS; x++)
      for (int y = 0; y < GRID_ROWS; y++)
        piece_states_[x][y].reset(x, y);
```

Add public method after `moveDumbOrthogonal`:
```cpp
  // ── Physics Orthogonal Move ──────────────────────────────────

  MoveError movePhysicsOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                  const PhysicsParams& params = PhysicsParams{}, bool skipValidation = false) {
    LOG_BOARD("movePhysicsOrthogonal: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS)
      return MoveError::OUT_OF_BOUNDS;
    if (fromX == toX && fromY == toY) return MoveError::SAME_POSITION;
    if (fromX != toX && fromY != toY) return MoveError::NOT_ORTHOGONAL;

    if (!skipValidation) {
      if (getPiece(fromX, fromY) == PIECE_NONE) return MoveError::NO_PIECE_AT_SOURCE;
      int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
      int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
      int8_t cx = fromX + dx, cy = fromY + dy;
      while (cx != toX || cy != toY) {
        if (getPiece(cx, cy) != PIECE_NONE) return MoveError::PATH_BLOCKED;
        cx += dx; cy += dy;
      }
    }

    // Build coil path
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    uint8_t path[GRID_COLS + GRID_ROWS][2];
    int path_len = 0;
    int8_t cx = fromX, cy = fromY;
    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;
      path[path_len][0] = cx;
      path[path_len][1] = cy;
      path_len++;
    }

    // Provide calibration data to physics engine
    if (cal_data_.valid) {
      physics_.setCalData(
        reinterpret_cast<const CalSensorData*>(cal_data_.sensors),
        NUM_HALL_SENSORS
      );
    }

    // Execute physics move
    PieceState& ps = piece_states_[fromX][fromY];
    ps.reset(fromX, fromY);  // start fresh from known position
    MoveError err = physics_.execute(ps, path, path_len, params);

    if (err == MoveError::NONE) {
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[toX][toY] = piece;
      piece_states_[toX][toY] = ps;
      LOG_BOARD("movePhysicsOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
    } else {
      LOG_BOARD("movePhysicsOrthogonal FAILED: %d", (int)err);
    }
    return err;
  }
```

- [ ] **Step 2: Verify `CalSensorData` layout matches `CalSensor`**

`CalSensorData` has `{float baseline_mean, float piece_mean}`. `CalSensor` has `{float baseline_mean, float baseline_stddev, float piece_mean, ...}`. The `reinterpret_cast` won't work because the fields don't align. Instead, add a conversion in Board that builds a `CalSensorData` array:

Add to private members:
```cpp
  CalSensorData cal_sensor_data_[NUM_HALL_SENSORS];
```

Add a helper:
```cpp
  void updatePhysicsCalData() {
    if (!cal_data_.valid) return;
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      cal_sensor_data_[i].baseline_mean = cal_data_.sensors[i].baseline_mean;
      cal_sensor_data_[i].piece_mean = cal_data_.sensors[i].piece_mean;
    }
    physics_.setCalData(cal_sensor_data_, NUM_HALL_SENSORS);
  }
```

Call `updatePhysicsCalData()` at end of `calibrate()` (after `cal_data_.valid = true`) and in `movePhysicsOrthogonal` instead of the `reinterpret_cast` block:
```cpp
    if (cal_data_.valid) {
      updatePhysicsCalData();
    }
```

- [ ] **Step 3: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 4: Commit**

```bash
git add firmware/board.h
git commit -m "feat: wire PhysicsMove into Board with movePhysicsOrthogonal"
```

---

### Task 6: Add API command and serial handler

**Files:**
- Modify: `firmware/api.h`
- Modify: `firmware/serial_server.h`

- [ ] **Step 1: Add `MovePhysicsRequest` to `api.h` (after `MoveDumbRequest`)**

```cpp
struct MovePhysicsRequest {
  uint8_t from_x;
  uint8_t from_y;
  uint8_t to_x;
  uint8_t to_y;
  float force_k;
  float force_epsilon;
  float falloff_exp;
  float voltage_scale;
  float friction_static;
  float friction_kinetic;
  float target_velocity;
  float target_accel;
  float sensor_k;
  float sensor_falloff;
  float sensor_threshold;
  uint16_t max_duration_ms;
};
```

Add API_COMMAND line:
```
// API_COMMAND(move_physics, POST, /api/move_physics, MovePhysicsRequest, MoveResponse)
```

- [ ] **Step 2: Add `handleMovePhysics` to `serial_server.h`**

```cpp
inline String handleMovePhysics(Board& board, const String& params) {
  uint8_t fromX = jsonGet(params, "from_x").toInt();
  uint8_t fromY = jsonGet(params, "from_y").toInt();
  uint8_t toX = jsonGet(params, "to_x").toInt();
  uint8_t toY = jsonGet(params, "to_y").toInt();

  PhysicsParams p;
  String v;
  if ((v = jsonGet(params, "force_k")).length())          p.force_k = v.toFloat();
  if ((v = jsonGet(params, "force_epsilon")).length())     p.force_epsilon = v.toFloat();
  if ((v = jsonGet(params, "falloff_exp")).length())       p.falloff_exp = v.toFloat();
  if ((v = jsonGet(params, "voltage_scale")).length())     p.voltage_scale = v.toFloat();
  if ((v = jsonGet(params, "friction_static")).length())   p.friction_static = v.toFloat();
  if ((v = jsonGet(params, "friction_kinetic")).length())  p.friction_kinetic = v.toFloat();
  if ((v = jsonGet(params, "target_velocity")).length())   p.target_velocity = v.toFloat();
  if ((v = jsonGet(params, "target_accel")).length())      p.target_accel = v.toFloat();
  if ((v = jsonGet(params, "sensor_k")).length())          p.sensor_k = v.toFloat();
  if ((v = jsonGet(params, "sensor_falloff")).length())    p.sensor_falloff = v.toFloat();
  if ((v = jsonGet(params, "sensor_threshold")).length())  p.sensor_threshold = v.toFloat();
  if ((v = jsonGet(params, "max_duration_ms")).length())   p.max_duration_ms = v.toInt();

  MoveError err = board.movePhysicsOrthogonal(fromX, fromY, toX, toY, p);
  MoveResponse res = { err == MoveError::NONE, err };
  return res.toJson();
}
```

Register in `SerialServer` constructor:
```cpp
    on("move_physics", handleMovePhysics);
```

- [ ] **Step 3: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 4: Commit**

```bash
git add firmware/api.h firmware/serial_server.h
git commit -m "feat: add move_physics API command with all PhysicsParams"
```

---

### Task 7: Regenerate TypeScript API and update frontend

**Files:**
- Run: `codegen/generate.py`
- Modify: `frontend/src/widgets/HexapawnWidget.tsx`

- [ ] **Step 1: Run codegen**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && python3 codegen/generate.py
```

Verify `movePhysics` appears in `frontend/src/generated/api.ts` and `moveSmart` is gone.

- [ ] **Step 2: Update `HexapawnWidget.tsx` — replace smart move with physics move**

Replace the import:
```tsx
import { getBoardState, moveDumb, movePhysics, type GetBoardStateResponse } from "../generated/api";
```

Replace `DEFAULT_SMART_PARAMS` with:
```tsx
const DEFAULT_PHYSICS_PARAMS = {
  force_k: 5.0,
  force_epsilon: 0.5,
  falloff_exp: 2.0,
  voltage_scale: 1.0,
  friction_static: 1.0,
  friction_kinetic: 3.0,
  target_velocity: 3.0,
  target_accel: 10.0,
  sensor_k: 500.0,
  sensor_falloff: 2.0,
  sensor_threshold: 50.0,
  max_duration_ms: 5000,
};
```

Replace `useSmart`/`smartParams` state with:
```tsx
const [usePhysics, setUsePhysics] = useState(false);
const [showParams, setShowParams] = useState(false);
const [physicsParams, setPhysicsParams] = useState(DEFAULT_PHYSICS_PARAMS);
```

In `handleClick`, replace the move call:
```tsx
const res = usePhysics
  ? await movePhysics({ ...moveParams, ...physicsParams })
  : await moveDumb(moveParams);
```

Replace the `updateParam` function:
```tsx
const updateParam = (key: keyof typeof physicsParams, val: string) => {
  setPhysicsParams(p => ({ ...p, [key]: parseFloat(val) || 0 }));
};
```

Update the toggle label from "Smart" to "Physics", and the status message from "smart" to "physics".

In the tune panel, replace `DEFAULT_SMART_PARAMS` references with `DEFAULT_PHYSICS_PARAMS` and `smartParams` with `physicsParams`. Use `step="0.1"` for float inputs:
```tsx
<input
  type="number"
  step={typeof physicsParams[key] === 'number' && key !== 'max_duration_ms' ? 0.1 : 1}
  value={physicsParams[key]}
  onChange={e => updateParam(key, e.target.value)}
  style={{ ...inputStyle, width: 70 }}
/>
```

- [ ] **Step 3: Type-check frontend**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32/frontend && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add frontend/src/generated/api.ts frontend/src/widgets/HexapawnWidget.tsx
git commit -m "feat: update frontend with physics move toggle and param tuning"
```

---

### Task 8: Compile, flash, and verify

**Files:** None (verification only)

- [ ] **Step 1: Full compile**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 2: Flash and test**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && bash scripts/dev_serial.sh`

Test checklist:
- Dumb move still works (click piece, click destination with Physics unchecked)
- Physics move activates (check Physics, click piece, click destination)
- Serial log shows `physics:` prefixed messages during physics move
- Tune panel opens and values can be changed
- Calibration still works

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "feat: physics-based piece movement with tunable params"
```
