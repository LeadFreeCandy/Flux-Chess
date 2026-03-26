# Physics Auto-Tuning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automated 4-phase tuning routine that discovers PhysicsParams from real board measurements and returns recommended values.

**Architecture:** Single `tunePhysics()` method in board.h runs all phases sequentially, streams LOG_BOARD progress, returns JSON with raw measurements and recommended PhysicsParams. Frontend adds "Tune Physics" button and "Apply Recommended" flow.

**Tech Stack:** C++ / Arduino on ESP32-S3, React/TypeScript frontend, Python codegen

---

### Task 1: Replace tunePhysics with phase structure and helpers

**Files:**
- Modify: `firmware/board.h`

- [ ] **Step 1: Replace the existing `tunePhysics` method (starting at the `// ── Physics Tuning` comment) with the new implementation containing constants, helpers, and the 4-phase structure**

Replace everything from `// ── Physics Tuning` up to and including the closing `}` of `tunePhysics` with:

```cpp
  // ── Physics Tuning ────────────────────────────────────────────
  // Automated 4-phase tuning: sensor fit, static friction, force profile, verification.
  // Place piece at (3,3) before calling.

  static constexpr int TUNE_NUM_REPS = 5;
  static constexpr int TUNE_FRICTION_PULSE_MS = 100;
  static constexpr int TUNE_FRICTION_DUTY_STEP = 10;
  static constexpr int TUNE_SETTLE_MS = 50;
  static constexpr uint8_t TUNE_X = 3;
  static constexpr uint8_t TUNE_Y = 3;
  static constexpr int TUNE_DURATIONS[] = {10, 25, 50, 100, 200};
  static constexpr int TUNE_NUM_DURATIONS = 5;

  String tunePhysics() {
    LOG_BOARD("TUNE: start — expecting piece at (%d,%d)", TUNE_X, TUNE_Y);

    memset(pieces_, PIECE_NONE, sizeof(pieces_));
    pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;

    uint8_t si = sensorForGrid(TUNE_X, TUNE_Y);
    int8_t coil_bit = coordToBit(TUNE_X + 1, TUNE_Y);  // adjacent coil for testing

    // Get baseline
    float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2030.0f;

    // ── Phase 1: Sensor curve fitting ──
    LOG_BOARD("TUNE: phase1 - sensor curve fitting");
    uint16_t readings_d0[TUNE_NUM_REPS];
    uint16_t readings_d1[TUNE_NUM_REPS];
    uint16_t readings_d2[TUNE_NUM_REPS];

    // d=0: piece at sensor center
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      readings_d0[i] = hw_.readSensor(si);
      delay(10);
    }

    // d=1: move piece one unit away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 1, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d1[i] = hw_.readSensor(si);
      calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // d=2: move piece two units away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d2[i] = hw_.readSensor(si);
      calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // Sort arrays for median
    tuneSort(readings_d0, TUNE_NUM_REPS);
    tuneSort(readings_d1, TUNE_NUM_REPS);
    tuneSort(readings_d2, TUNE_NUM_REPS);

    float str0 = baseline - readings_d0[TUNE_NUM_REPS / 2];
    float str1 = baseline - readings_d1[TUNE_NUM_REPS / 2];
    float str2 = baseline - readings_d2[TUNE_NUM_REPS / 2];

    // Fit sensor_k and sensor_falloff
    float eps = 0.3f;  // use force_epsilon as shared epsilon
    float fitted_sensor_k = (str0 > 0) ? str0 * eps : 500.0f;
    float fitted_sensor_falloff = 2.0f;  // default
    if (str1 > 1.0f && str2 > 1.0f) {
      // Solve: str1/str2 = (2^falloff + eps) / (1 + eps)
      float ratio = str1 / str2;
      // Approximate: try falloff values and pick best fit
      float best_err = 999;
      for (float f = 1.0f; f <= 4.0f; f += 0.1f) {
        float predicted_ratio = (powf(2.0f, f) + eps) / (1.0f + eps);
        float err = fabsf(predicted_ratio - ratio);
        if (err < best_err) { best_err = err; fitted_sensor_falloff = f; }
      }
    }

    LOG_BOARD("TUNE: sensor fit: k=%.1f falloff=%.2f (str: d0=%.0f d1=%.0f d2=%.0f)",
              fitted_sensor_k, fitted_sensor_falloff, str0, str1, str2);

    // ── Phase 2: Static friction measurement ──
    LOG_BOARD("TUNE: phase2 - static friction");
    uint8_t threshold_duties[TUNE_NUM_REPS];

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Ensure piece is centered
      calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);  // no-op if already there
      delay(TUNE_SETTLE_MS);
      uint16_t pre = hw_.readSensor(si);

      uint8_t found_duty = 255;
      for (int d = TUNE_FRICTION_DUTY_STEP; d <= 255; d += TUNE_FRICTION_DUTY_STEP) {
        hw_.pulseBit((uint8_t)coil_bit, TUNE_FRICTION_PULSE_MS, (uint8_t)d);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        if (abs((int)post - (int)pre) > 20) {
          found_duty = (uint8_t)d;
          LOG_BOARD("TUNE: friction rep %d: moved at duty=%d (pre=%d post=%d)", rep, d, pre, post);
          break;
        }

        // Re-center for next duty step
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);  // pull back if it drifted
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        pieces_[TUNE_X + 1][TUNE_Y] = PIECE_NONE;
        delay(TUNE_SETTLE_MS);
        pre = hw_.readSensor(si);  // re-baseline
      }

      threshold_duties[rep] = found_duty;

      // Re-center piece
      // Find where piece is and move back
      for (int x = 0; x < GRID_COLS; x++)
        for (int y = 0; y < GRID_ROWS; y++)
          if (pieces_[x][y] != PIECE_NONE && (x != TUNE_X || y != TUNE_Y)) {
            calMove(x, y, TUNE_X, TUNE_Y);
          }
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    tuneSort(threshold_duties, TUNE_NUM_REPS);
    uint8_t median_duty = threshold_duties[TUNE_NUM_REPS / 2];

    // Convert to friction_static using force model at d=1.0
    float default_force_k = 10.0f;
    float force_at_1 = default_force_k * 1.0f / (powf(1.0f, 2.0f) + eps);
    float fitted_friction_static = force_at_1 * (median_duty / 255.0f);

    LOG_BOARD("TUNE: static friction: median_duty=%d friction_static=%.2f", median_duty, fitted_friction_static);

    // ── Phase 3: Coil force profiling ──
    LOG_BOARD("TUNE: phase3 - force profiling");
    float displacements[TUNE_NUM_DURATIONS];

    for (int di = 0; di < TUNE_NUM_DURATIONS; di++) {
      float disp_sum = 0;
      int valid_reps = 0;

      for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
        // Ensure centered
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);
        delay(TUNE_SETTLE_MS);

        uint16_t pre = hw_.readSensor(si);
        hw_.pulseBit((uint8_t)coil_bit, TUNE_DURATIONS[di], 255);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        float strength_pre = baseline - pre;
        float strength_post = baseline - post;

        // Estimate displacement using sensor model
        float d_pre = (strength_pre > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_pre - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 0.0f;
        float d_post = (strength_post > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_post - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 3.0f;

        float disp = d_post - d_pre;
        if (disp > 0) {
          disp_sum += disp;
          valid_reps++;
        }

        LOG_BOARD("TUNE: force dur=%dms rep=%d pre=%d post=%d disp=%.2f",
                  TUNE_DURATIONS[di], rep, pre, post, disp);

        // Re-center
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
        calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      }

      displacements[di] = (valid_reps > 0) ? disp_sum / valid_reps : 0;
      LOG_BOARD("TUNE: force dur=%dms avg_disp=%.3f", TUNE_DURATIONS[di], displacements[di]);
    }

    // Fit force_k: at d=1, F = force_k * 1/(1+eps).
    // displacement ≈ 0.5 * (F - friction_static) * t^2 for short pulses
    // Use the 100ms data point for fitting
    float t_fit = 0.1f;  // 100ms
    float disp_fit = displacements[3];  // index 3 = 100ms
    float fitted_force_k = default_force_k;
    if (disp_fit > 0.01f) {
      // disp = 0.5 * (F - fric_s) * t^2, F = fk * 1/(1+eps)
      // fk = (2*disp/t^2 + fric_s) * (1+eps)
      fitted_force_k = (2.0f * disp_fit / (t_fit * t_fit) + fitted_friction_static) * (1.0f + eps);
    }

    // Fit friction_kinetic from ratio of displacements
    // Longer pulses show diminishing returns due to friction limiting speed
    float fitted_friction_kinetic = 2.0f;  // default
    if (displacements[4] > 0.01f && displacements[2] > 0.01f) {
      // ratio of 200ms/50ms displacement — if purely kinematic would be 16x, less = more friction
      float ratio = displacements[4] / displacements[2];
      // friction_kinetic ≈ force / terminal_velocity, estimate from saturation
      if (ratio < 10.0f && ratio > 1.0f) {
        float terminal_v = displacements[4] / 0.2f;  // rough v from displacement/time
        float f_net = fitted_force_k * 1.0f / (1.0f + eps);
        fitted_friction_kinetic = f_net / fmaxf(terminal_v, 0.1f);
      }
    }

    LOG_BOARD("TUNE: fitted force_k=%.2f friction_kinetic=%.2f", fitted_force_k, fitted_friction_kinetic);

    // ── Phase 4: Move verification ──
    LOG_BOARD("TUNE: phase4 - verification");
    int success_count = 0;
    uint16_t dest_readings[TUNE_NUM_REPS];
    unsigned long elapsed_ms[TUNE_NUM_REPS];

    PhysicsParams test_params;
    test_params.force_k = fitted_force_k;
    test_params.force_epsilon = eps;
    test_params.friction_static = fitted_friction_static;
    test_params.friction_kinetic = fitted_friction_kinetic;
    test_params.sensor_k = fitted_sensor_k;
    test_params.sensor_falloff = fitted_sensor_falloff;
    test_params.manual_baseline = baseline;
    test_params.manual_piece_mean = readings_d0[TUNE_NUM_REPS / 2];

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Move piece to (5,3)
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      pieces_[TUNE_X][TUNE_Y] = PIECE_NONE;
      pieces_[TUNE_X + 2][TUNE_Y] = PIECE_WHITE;
      delay(TUNE_SETTLE_MS);

      // Attempt physics move back
      unsigned long t0 = millis();
      MoveError err = movePhysicsOrthogonal(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y, test_params, true);
      elapsed_ms[rep] = millis() - t0;

      delay(100);
      dest_readings[rep] = hw_.readSensor(si);

      bool arrived = (err == MoveError::NONE) && (dest_readings[rep] < baseline - 100);
      if (arrived) success_count++;

      LOG_BOARD("TUNE: verify rep=%d err=%d t=%lums reading=%d %s",
                rep, (int)err, elapsed_ms[rep], dest_readings[rep], arrived ? "OK" : "MISS");

      // Re-center
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    LOG_BOARD("TUNE: verification: %d/%d success", success_count, TUNE_NUM_REPS);

    // ── Build result JSON ──
    String j = "{\"sensor_fit\":{\"sensor_k\":";
    j += String(fitted_sensor_k, 1);
    j += ",\"sensor_falloff\":";
    j += String(fitted_sensor_falloff, 2);
    j += ",\"readings_d0\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d0[i]); }
    j += "],\"readings_d1\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d1[i]); }
    j += "],\"readings_d2\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d2[i]); }
    j += "]},\"static_friction\":{\"threshold_duties\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(threshold_duties[i]); }
    j += "],\"median_duty\":";
    j += String(median_duty);
    j += ",\"friction_static\":";
    j += String(fitted_friction_static, 2);
    j += "},\"force_profile\":{\"durations_ms\":[10,25,50,100,200],\"displacements\":[";
    for (int i = 0; i < TUNE_NUM_DURATIONS; i++) { if (i) j += ","; j += String(displacements[i], 3); }
    j += "],\"force_k\":";
    j += String(fitted_force_k, 2);
    j += ",\"friction_kinetic\":";
    j += String(fitted_friction_kinetic, 2);
    j += "},\"verification\":{\"success_count\":";
    j += String(success_count);
    j += ",\"total\":";
    j += String(TUNE_NUM_REPS);
    j += ",\"dest_readings\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(dest_readings[i]); }
    j += "],\"elapsed_ms\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(elapsed_ms[i]); }
    j += "]},\"recommended\":{";
    j += "\"force_k\":"; j += String(fitted_force_k, 2);
    j += ",\"force_epsilon\":"; j += String(eps, 2);
    j += ",\"falloff_exp\":2.0";
    j += ",\"voltage_scale\":1.0";
    j += ",\"friction_static\":"; j += String(fitted_friction_static, 2);
    j += ",\"friction_kinetic\":"; j += String(fitted_friction_kinetic, 2);
    j += ",\"target_velocity\":5.0";
    j += ",\"target_accel\":20.0";
    j += ",\"sensor_k\":"; j += String(fitted_sensor_k, 1);
    j += ",\"sensor_falloff\":"; j += String(fitted_sensor_falloff, 2);
    j += ",\"sensor_threshold\":50.0";
    j += ",\"manual_baseline\":"; j += String(baseline, 1);
    j += ",\"manual_piece_mean\":"; j += String((float)readings_d0[TUNE_NUM_REPS / 2], 1);
    j += ",\"max_duration_ms\":5000";
    j += "}}";

    LOG_BOARD("TUNE: complete");
    initDefaultBoard();
    return j;
  }
```

- [ ] **Step 2: Add the `tuneSort` helper in the private section (near `calMove`)**

```cpp
  // Insertion sort for small arrays (used by tuning)
  static void tuneSort(uint16_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint16_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }

  static void tuneSort(uint8_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint8_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }
```

- [ ] **Step 3: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 4: Commit**

```bash
git add firmware/board.h
git commit -m "feat: replace tunePhysics with 4-phase auto-tuning system"
```

---

### Task 2: Update serial handler and API

**Files:**
- Modify: `firmware/serial_server.h`
- Modify: `firmware/api.h`

- [ ] **Step 1: Update `handleTunePhysics` in serial_server.h to call the new no-arg `tunePhysics()`**

Replace the existing `handleTunePhysics` function:
```cpp
inline String handleTunePhysics(Board& board, const String& params) {
  return board.tunePhysics();
}
```

- [ ] **Step 2: Update api.h — change `TunePhysicsRequest` to empty struct**

Ensure `tune_physics` API_COMMAND uses an empty request. It should already be:
```
// API_COMMAND(tune_physics, POST, /api/tune_physics, MovePhysicsRequest, GetCalibrationResponse)
```
Change to:
```
// API_COMMAND(tune_physics, POST, /api/tune_physics, CalibrateRequest, GetCalibrationResponse)
```
(CalibrateRequest is already an empty struct)

- [ ] **Step 3: Run codegen**

```bash
cd /Users/samir/Documents/projects/fluxchess/esp32 && python3 codegen/generate.py
```

- [ ] **Step 4: Compile firmware**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 5: Commit**

```bash
git add firmware/serial_server.h firmware/api.h frontend/src/generated/api.ts
git commit -m "feat: update tune_physics API to no-arg command"
```

---

### Task 3: Add Tune Physics button and Apply Recommended to frontend

**Files:**
- Modify: `frontend/src/widgets/CalibrationWidget.tsx`
- Modify: `frontend/src/widgets/HexapawnWidget.tsx`

- [ ] **Step 1: In CalibrationWidget.tsx, add a "Tune Physics" button in the idle state, alongside "Start Calibration" and "View Stored"**

Add `tunePhysics` import from the generated API. Add state for tuning results. Add a button that calls `tunePhysics({})`, streams TUNE: logs, and on completion fetches and displays results with an "Apply Recommended" button.

The "Apply Recommended" button needs to communicate with HexapawnWidget. The simplest approach: store recommended params in `localStorage` and have HexapawnWidget read them. Add:

In CalibrationWidget, after tuning completes:
```tsx
const applyRecommended = (recommended: Record<string, number>) => {
  localStorage.setItem('fluxchess_physics_params', JSON.stringify(recommended));
  onStatus("Recommended params saved — switch to Hexapawn and enable Physics");
};
```

Button in the done state:
```tsx
<button onClick={() => applyRecommended(tuneResult.recommended)} style={{ ...btnStyle, flex: 1 }}>
  Apply Recommended
</button>
```

In HexapawnWidget, on mount check localStorage:
```tsx
useEffect(() => {
  const saved = localStorage.getItem('fluxchess_physics_params');
  if (saved) {
    try {
      const parsed = JSON.parse(saved);
      setPhysicsParams(p => ({ ...p, ...parsed }));
    } catch {}
  }
}, []);
```

- [ ] **Step 2: Type-check frontend**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32/frontend && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add frontend/src/widgets/CalibrationWidget.tsx frontend/src/widgets/HexapawnWidget.tsx
git commit -m "feat: add Tune Physics button with Apply Recommended flow"
```

---

### Task 4: Compile, flash, and verify

**Files:** None (verification only)

- [ ] **Step 1: Full compile**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" --build-path build firmware`
Expected: Compiles with no errors

- [ ] **Step 2: Flash and test**

Run: `cd /Users/samir/Documents/projects/fluxchess/esp32 && bash scripts/dev_serial.sh`

Test checklist:
- Place piece at (3,3)
- Click "Tune Physics" in Calibration widget
- Logs stream with TUNE: prefix showing all 4 phases
- After completion, "Apply Recommended" button appears
- Click Apply, switch to Hexapawn widget
- Physics params populated with recommended values
- Attempt a physics move with the recommended params

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "feat: physics auto-tuning with 4-phase measurement and recommended params"
```
