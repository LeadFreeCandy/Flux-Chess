# Sensor Diagnostics & Checkpoint Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add sensor-based diagnostic tracing and checkpoint verification to physics moves, with optional retry/recovery when the destination checkpoint fails.

**Architecture:** Sensor reads happen in the physics tick dead time at max ADC rate, tracking per-path-coil min readings. After a move completes, the destination sensor is checked. If it fails and retries are available, `moveDumb` pushes the piece forward from the last detected coil, then the physics move recurses for the remaining distance.

**Tech Stack:** ESP32 C++ (Arduino framework), Hall effect sensors via ADC, existing `Hardware::readSensor()` API.

**Spec:** `docs/superpowers/specs/2026-03-27-sensor-diagnostics-checkpoint-design.md`

---

### Task 1: Add MoveDiag data structures to api.h

**Files:**
- Modify: `firmware/api.h:131-135`

- [ ] **Step 1: Add CoilDiag and MoveDiag structs above MoveResponse**

Add these structs before `MoveResponse` (after line 130 in api.h):

```cpp
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
```

- [ ] **Step 2: Update MoveResponse to include optional diag**

Replace the existing `MoveResponse` struct:

```cpp
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
```

- [ ] **Step 3: Verify firmware compiles**

Run the Arduino IDE compile or equivalent build command. Expected: compiles with no errors (existing code still uses `MoveResponse{success, error}` aggregate init, which still works since `has_diag` defaults to false).

- [ ] **Step 4: Commit**

```bash
git add firmware/api.h
git commit -m "feat: add MoveDiag and CoilDiag structs for sensor diagnostics"
```

---

### Task 2: Add sensor sampling to PhysicsMove::execute()

**Files:**
- Modify: `firmware/physics.h:42-412`

The physics tick loop enforces a 10ms minimum via `delayMicroseconds(10000 - elapsed_us)` at line 109. We replace that passive wait with an active sensor-sampling loop.

- [ ] **Step 1: Add diag parameters and sensor helpers to execute()**

Change the `execute()` signature at line 51 to accept diag output and sensor info:

```cpp
MoveError execute(PieceState& piece, const float path_mm[][2], int path_len,
                  const PhysicsParams& params,
                  MoveDiag* diag = nullptr,
                  const uint8_t* path_sensors = nullptr, int num_path_sensors = 0,
                  const float* sensor_thresholds = nullptr) {
```

- [ ] **Step 2: Initialize diag tracking at the start of execute()**

After the existing variable initialization (after line 92, before the LOG_BOARD "physics: start" line), add:

```cpp
    // Sensor diagnostic tracking
    if (diag) {
      diag->num_coils = (uint8_t)fminf(path_len, MAX_DIAG_COILS);
      for (int i = 0; i < diag->num_coils; i++) {
        diag->coils[i].sensor_idx = (path_sensors && i < num_path_sensors) ? path_sensors[i] : 0;
        diag->coils[i].min_reading = 0xFFFF;
        diag->coils[i].detected = false;
        diag->coils[i].arrival_reading = 0;
      }
    }
```

- [ ] **Step 3: Replace delayMicroseconds with sensor-sampling loop**

Replace the tick timing block (lines 105-112):

```cpp
      // 1. dt — enforce minimum 10ms tick (100Hz), sample sensors in the gap
      unsigned long now_us = micros();
      unsigned long elapsed_us = now_us - last_tick_us;
      if (elapsed_us < 10000) {
        unsigned long deadline_us = last_tick_us + 10000;
        // Sample path sensors as fast as possible in the dead time
        while (micros() < deadline_us) {
          if (diag && path_sensors && num_path_sensors > 0) {
            for (int si = 0; si < diag->num_coils && si < num_path_sensors; si++) {
              uint16_t r = hw_.readSensor(path_sensors[si]);
              if (r < diag->coils[si].min_reading) diag->coils[si].min_reading = r;
              if (sensor_thresholds && r < sensor_thresholds[si]) diag->coils[si].detected = true;
            }
          } else {
            delayMicroseconds(100);  // no sensors to sample, just wait
          }
        }
        now_us = micros();
        elapsed_us = now_us - last_tick_us;
      }
```

- [ ] **Step 4: Add arrival readings after the main loop**

After the main while loop ends (after the timeout LOG_BOARD at line 339, before the final return), add arrival reads:

```cpp
    // Read final sensor values for diagnostics
    if (diag && path_sensors && num_path_sensors > 0) {
      delay(50);  // settle time
      for (int i = 0; i < diag->num_coils && i < num_path_sensors; i++) {
        diag->coils[i].arrival_reading = hw_.readSensor(path_sensors[i]);
      }
    }
```

Also add the same block just before the `return MoveError::NONE` inside the arrival check (around line 322), so successful moves also get arrival readings.

- [ ] **Step 5: Verify firmware compiles**

All existing callers pass no diag pointer, so they get the default `nullptr` behavior — no sensor reads, no overhead. Expected: compiles clean.

- [ ] **Step 6: Commit**

```bash
git add firmware/physics.h
git commit -m "feat: add sensor sampling to physics tick dead time"
```

---

### Task 3: Wire up diagnostics in movePhysicsOrthogonal()

**Files:**
- Modify: `firmware/board.h:117-167`

- [ ] **Step 1: Add max_retry_attempts parameter and diag output**

Update the signature at line 117:

```cpp
  MoveError movePhysicsOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                  bool skipValidation = false, int max_retry_attempts = 0,
                                  MoveDiag* out_diag = nullptr) {
```

- [ ] **Step 2: Build path sensor list and thresholds before executing physics**

After the path_mm building loop (after line 150, before the `// Execute physics move` comment), add:

```cpp
    // Build sensor list and thresholds for path coils
    uint8_t path_sensors[MAX_DIAG_COILS];
    float sensor_thresholds[MAX_DIAG_COILS];
    int num_path_sensors = 0;
    {
      int8_t sx = fromX, sy = fromY;
      for (int i = 0; i < path_len && num_path_sensors < MAX_DIAG_COILS; i++) {
        sx += stepX; sy += stepY;
        uint8_t si = sensorForGrid(sx, sy);
        float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2030.0f;
        float piece_mean = cal_data_.valid ? cal_data_.sensors[si].piece_mean : 1700.0f;
        path_sensors[num_path_sensors] = si;
        sensor_thresholds[num_path_sensors] = (baseline + piece_mean) / 2.0f;
        num_path_sensors++;
      }
    }

    // Execute physics move with diagnostics
    MoveDiag diag;
```

- [ ] **Step 3: Pass diag into physics execute and add checkpoint**

Replace the physics execute call and the result handling (lines 152-166):

```cpp
    PieceState& ps = piece_states_[fromX][fromY];
    ps.reset(fromX * GRID_TO_MM, fromY * GRID_TO_MM);
    MoveError err = physics_.execute(ps, path_mm, path_len, params,
                                     &diag, path_sensors, num_path_sensors, sensor_thresholds);

    // Checkpoint: verify destination sensor detects the piece
    if (num_path_sensors > 0) {
      int dest_idx = num_path_sensors - 1;
      uint16_t dest_reading = hw_.readSensor(path_sensors[dest_idx]);
      diag.coils[dest_idx].arrival_reading = dest_reading;
      diag.checkpoint_ok = (dest_reading < sensor_thresholds[dest_idx]);
    } else {
      diag.checkpoint_ok = true;  // no sensors, assume OK
    }

    // Log diagnostic summary
    {
      String log = "physics: diag";
      for (int i = 0; i < diag.num_coils; i++) {
        log += " coil" + String(i) + "=" + (diag.coils[i].detected ? "OK" : "MISS");
        log += "(" + String(diag.coils[i].min_reading) + ")";
      }
      log += " checkpoint=" + String(diag.checkpoint_ok ? "OK" : "FAIL");
      LOG_BOARD("%s", log.c_str());
    }

    if (err == MoveError::NONE && diag.checkpoint_ok) {
      // Success — update board state
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[toX][toY] = piece;
      piece_states_[toX][toY] = ps;
      LOG_BOARD("movePhysicsOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
      if (out_diag) *out_diag = diag;
      return MoveError::NONE;
    }

    // Checkpoint failed — attempt recovery if retries remain
    if (!diag.checkpoint_ok && max_retry_attempts > 0) {
      LOG_BOARD("physics: checkpoint FAIL, attempting recovery (retries left: %d)", max_retry_attempts);

      // Find last coil that detected the piece (scan backwards from destination)
      int last_ok = -1;
      for (int i = diag.num_coils - 1; i >= 0; i--) {
        if (diag.coils[i].detected) { last_ok = i; break; }
      }

      // Recovery target: the coil after last_ok (or the first coil if none detected)
      int recovery_idx = (last_ok >= 0) ? last_ok + 1 : 0;
      if (recovery_idx >= path_len) {
        // All coils detected but checkpoint still failed — piece overshot?
        LOG_BOARD("physics: all coils detected but checkpoint failed, no recovery possible");
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return err != MoveError::NONE ? err : MoveError::COIL_FAILURE;
      }

      // Compute grid position of recovery target
      uint8_t recX = fromX + stepX * (recovery_idx + 1);
      uint8_t recY = fromY + stepY * (recovery_idx + 1);

      // Compute grid position of last-ok coil (or source if none detected)
      uint8_t lastX = (last_ok >= 0) ? fromX + stepX * (last_ok + 1) : fromX;
      uint8_t lastY = (last_ok >= 0) ? fromY + stepY * (last_ok + 1) : fromY;

      LOG_BOARD("physics: recovery: last detected=(%d,%d), pushing to (%d,%d) via moveDumb",
                lastX, lastY, recX, recY);

      // Update board state: piece is at last-ok position
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[lastX][lastY] = piece;

      // Push piece to recovery target with moveDumb
      MoveError dumbErr = moveDumbOrthogonal(lastX, lastY, recX, recY, true);
      if (dumbErr != MoveError::NONE) {
        LOG_BOARD("physics: recovery moveDumb failed: %d", (int)dumbErr);
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return dumbErr;
      }

      // Verify piece arrived at recovery target
      uint8_t recSensor = sensorForGrid(recX, recY);
      delay(50);
      uint16_t recReading = hw_.readSensor(recSensor);
      float recBaseline = cal_data_.valid ? cal_data_.sensors[recSensor].baseline_mean : 2030.0f;
      float recPieceMean = cal_data_.valid ? cal_data_.sensors[recSensor].piece_mean : 1700.0f;
      float recThreshold = (recBaseline + recPieceMean) / 2.0f;

      if (recReading >= recThreshold) {
        LOG_BOARD("physics: recovery verification FAILED at (%d,%d) reading=%d threshold=%.0f",
                  recX, recY, recReading, recThreshold);
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return MoveError::COIL_FAILURE;
      }

      LOG_BOARD("physics: recovery verified at (%d,%d), retrying physics to (%d,%d)",
                recX, recY, toX, toY);

      // Recurse: physics move from recovery position to original destination
      diag.retries_used++;
      MoveDiag retry_diag;
      MoveError retryErr = movePhysicsOrthogonal(recX, recY, toX, toY,
                                                  true, max_retry_attempts - 1, &retry_diag);
      retry_diag.retries_used += diag.retries_used;
      if (out_diag) *out_diag = retry_diag;
      return retryErr;
    }

    // No recovery — return result with diag
    if (out_diag) *out_diag = diag;
    if (err != MoveError::NONE) {
      LOG_BOARD("movePhysicsOrthogonal FAILED: %d", (int)err);
    }
    return err;
  }
```

- [ ] **Step 4: Verify firmware compiles**

Existing callers of `movePhysicsOrthogonal` use default params, so they compile unchanged. Expected: clean compile.

- [ ] **Step 5: Commit**

```bash
git add firmware/board.h
git commit -m "feat: wire sensor diagnostics and checkpoint recovery into movePhysicsOrthogonal"
```

---

### Task 4: Thread max_retry_attempts through movePiece and hexapawnPlay

**Files:**
- Modify: `firmware/board.h:423-457` (movePiece)
- Modify: `firmware/board.h:396-401` (hexapawnPlay AI move call)

- [ ] **Step 1: Add max_retry_attempts to movePiece**

Update `movePiece` signature at line 423:

```cpp
  MoveError movePiece(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                      int max_retry_attempts = 0) {
```

- [ ] **Step 2: Pass max_retry_attempts through moveOrthogonalClearing**

Update `moveOrthogonalClearing` signature at line 510:

```cpp
  MoveError moveOrthogonalClearing(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                   int depth = 0, int max_retry_attempts = 0) {
```

Inside `moveOrthogonalClearing`, find the line that calls the actual move (it dispatches to either physics or dumb). Search for the `use_physics_moves_` branch and pass `max_retry_attempts` to `movePhysicsOrthogonal`. The call should look like:

```cpp
    if (use_physics_moves_) {
      return movePhysicsOrthogonal(fromX, fromY, toX, toY, false, max_retry_attempts);
    } else {
      return moveDumbOrthogonal(fromX, fromY, toX, toY);
    }
```

- [ ] **Step 3: Thread through all movePiece -> moveOrthogonalClearing calls**

In `movePiece`, update all calls to `moveOrthogonalClearing` to pass `max_retry_attempts`:

```cpp
    if (fromX != toX && fromY != toY) {
      // ... Manhattan routing ...
      if (obstX1st <= obstY1st) {
        MoveError err = moveOrthogonalClearing(fromX, fromY, toX, fromY, 0, max_retry_attempts);
        if (err != MoveError::NONE) return err;
        return moveOrthogonalClearing(toX, fromY, toX, toY, 0, max_retry_attempts);
      } else {
        MoveError err = moveOrthogonalClearing(fromX, fromY, fromX, toY, 0, max_retry_attempts);
        if (err != MoveError::NONE) return err;
        return moveOrthogonalClearing(fromX, toY, toX, toY, 0, max_retry_attempts);
      }
    }
    return moveOrthogonalClearing(fromX, fromY, toX, toY, 0, max_retry_attempts);
```

Also thread through the recursive `moveOrthogonalClearing` calls for obstacle clearing:

```cpp
      MoveError err = moveOrthogonalClearing(bx, by, tx, ty, depth + 1, max_retry_attempts);
```

- [ ] **Step 4: Update hexapawnPlay to use retries for AI moves**

At line 401 in `hexapawnPlay()`, change:

```cpp
        MoveError err = movePiece(gfx, gfy, gtx, gty, 2);
```

- [ ] **Step 5: Verify firmware compiles**

Expected: clean compile. All existing callers use default `max_retry_attempts = 0`.

- [ ] **Step 6: Commit**

```bash
git add firmware/board.h
git commit -m "feat: thread max_retry_attempts through movePiece and hexapawnPlay"
```

---

### Task 5: Add max_retries to serial API commands

**Files:**
- Modify: `firmware/serial_server.h:56-74`

- [ ] **Step 1: Update handleMovePhysics to parse max_retries and return diag**

Replace the handler at lines 56-64:

```cpp
inline String handleMovePhysics(Board& board, const String& params) {
  uint8_t fromX = jsonGet(params, "from_x").toInt();
  uint8_t fromY = jsonGet(params, "from_y").toInt();
  uint8_t toX = jsonGet(params, "to_x").toInt();
  uint8_t toY = jsonGet(params, "to_y").toInt();
  int maxRetries = jsonGet(params, "max_retries").toInt();  // defaults to 0 if absent
  MoveDiag diag;
  MoveError err = board.movePhysicsOrthogonal(fromX, fromY, toX, toY, false, maxRetries, &diag);
  MoveResponse res = { err == MoveError::NONE, err, true, diag };
  return res.toJson();
}
```

- [ ] **Step 2: Update handleMovePiece to parse max_retries**

Replace the handler at lines 66-74:

```cpp
inline String handleMovePiece(Board& board, const String& params) {
  uint8_t fromX = jsonGet(params, "from_x").toInt();
  uint8_t fromY = jsonGet(params, "from_y").toInt();
  uint8_t toX = jsonGet(params, "to_x").toInt();
  uint8_t toY = jsonGet(params, "to_y").toInt();
  int maxRetries = jsonGet(params, "max_retries").toInt();
  MoveError err = board.movePiece(fromX, fromY, toX, toY, maxRetries);
  MoveResponse res = { err == MoveError::NONE, err };
  return res.toJson();
}
```

Note: `handleMovePiece` doesn't have direct access to `MoveDiag` since `movePiece` calls through `moveOrthogonalClearing`. The diag data is still logged. A future improvement could thread diag through `movePiece` too, but for now the serial log captures it.

- [ ] **Step 3: Verify firmware compiles**

Expected: clean compile.

- [ ] **Step 4: Commit**

```bash
git add firmware/serial_server.h
git commit -m "feat: add max_retries param to move_physics and move_piece API commands"
```

---

### Task 6: Update frontend TypeScript types

**Files:**
- Modify: `frontend/src/generated/api.ts` (or wherever types are auto-generated)

- [ ] **Step 1: Check how types are generated**

The frontend has auto-generated types from the firmware API. Check how they're generated:

```bash
ls frontend/src/generated/
grep -r "MoveResponse" frontend/src/
```

- [ ] **Step 2: Add diag types to match the new JSON**

Add to the generated types (or manually if not auto-generated):

```typescript
interface CoilDiag {
  sensor: number;
  min: number;
  detected: boolean;
  arrival: number;
}

interface MoveDiag {
  checkpoint_ok: boolean;
  retries_used: number;
  coils: CoilDiag[];
}

// MoveResponse now optionally includes diag
interface MoveResponse {
  success: boolean;
  error: number;
  diag?: MoveDiag;
}
```

- [ ] **Step 3: Commit**

```bash
git add frontend/src/generated/
git commit -m "feat: add MoveDiag types to frontend"
```

---

### Task 7: Test on hardware

**Files:**
- No file changes — manual testing

- [ ] **Step 1: Flash firmware to ESP32**

Build and upload the firmware via Arduino IDE or `dev serial` upload script.

- [ ] **Step 2: Test basic physics move with diagnostics**

Send via frontend or serial:

```json
{"method":"move_physics","params":{"from_x":0,"from_y":0,"to_x":0,"to_y":3}}
```

Expected response includes `diag` object with sensor readings for each coil along the path. Check that `min` readings are lower (stronger) for coils the piece passed over.

- [ ] **Step 3: Test move with max_retries**

```json
{"method":"move_physics","params":{"from_x":0,"from_y":0,"to_x":0,"to_y":3,"max_retries":2}}
```

Expected: same as above but with recovery if checkpoint fails.

- [ ] **Step 4: Test hexapawn game — verify AI moves use retries**

Start a hexapawn game via the frontend. Observe logs for `physics: diag` lines after each AI move. Verify `checkpoint=OK` appears.

- [ ] **Step 5: Test recovery by deliberately placing a piece badly**

If possible, nudge a piece mid-move to cause a checkpoint failure. Verify the recovery log appears and the piece is pushed to the correct position.
