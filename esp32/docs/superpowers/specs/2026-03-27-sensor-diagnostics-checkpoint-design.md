# Sensor Diagnostics & Checkpoint Recovery

## Problem

The physics simulation controls piece movement open-loop — it estimates position via integration but has no feedback on where the piece actually is. When the sim declares "arrived," the piece may have stalled, undershot, or overshot. There's no way to know without manually checking.

Meanwhile, 12 Hall effect sensors sit on the board unused during physics moves. They can detect piece presence at each major grid position with ~100us ADC read time.

## Goals

1. **Diagnostic trace**: During a physics move, continuously sample path sensors at max rate. Track whether the piece was detected at each coil along the path.
2. **Arrival checkpoint**: After the sim declares arrival, verify the destination sensor actually detects the piece.
3. **Recovery**: When a checkpoint fails and retries are allowed, use `moveDumb` to push the piece forward, verify, then recurse to complete the remaining path.

## Non-Goals

- Sensor-based position estimation or velocity correction (future work)
- Lateral drift detection (sensor range is too short — mm scale)
- Changing the physics control loop in any way

## Design

### Diagnostic Data Structure

```cpp
struct CoilDiag {
  uint8_t sensor_idx;       // which sensor (0-11)
  uint16_t min_reading;     // lowest ADC value seen (lower = stronger detection)
  bool detected;            // did min_reading cross detection threshold?
  uint16_t arrival_reading; // reading taken after move completes
};

struct MoveDiag {
  CoilDiag coils[9];        // max 9 coils in longest possible path
  uint8_t num_coils;        // actual number of path coils
  bool checkpoint_ok;       // destination sensor confirmed piece present
  uint8_t retries_used;     // how many recovery attempts were needed
};
```

### Sensor Sampling

During `PhysicsMove::execute()`, sensor reads happen in the dead time within each tick. The physics loop enforces a 10ms minimum tick via `delayMicroseconds`. During that wait, we continuously read the path sensors:

```
while (waiting for next tick) {
    for each sensor on path:
        reading = readSensor(sensor_idx)
        if reading < coil_diag[i].min_reading:
            coil_diag[i].min_reading = reading
        if reading < threshold:
            coil_diag[i].detected = true
    // ~100us per sensor, 1-3 sensors = 100-300us per loop
    // fits many iterations in the ~10ms gap
}
```

Only sensors along the move path are sampled (1-9 sensors depending on path length). Each sensor is identified by `sensorForGrid()` using the path coil coordinates.

Detection threshold uses calibration data when available: `threshold = (baseline_mean + piece_mean) / 2.0`. Falls back to hardcoded default (midpoint of 2030 and 1700 = 1865) when uncalibrated.

### Arrival Checkpoint

After the physics sim returns (either success or timeout):

1. Wait 50ms for the piece to settle
2. Read the destination sensor
3. Store as `arrival_reading` in the last `CoilDiag`
4. Set `checkpoint_ok = (arrival_reading < threshold)`

### Recovery Flow

`movePhysicsOrthogonal()` gains a `max_retry_attempts` parameter (default 0):

```
MoveError movePhysicsOrthogonal(fromX, fromY, toX, toY, max_retry_attempts = 0):
    result = physics.execute(piece, path, params)
    diag = collect diagnostic summary

    if diag.checkpoint_ok or max_retry_attempts == 0:
        return result with diag

    // Recovery: find where the piece stalled
    last_ok = last coil index where diag.coils[i].detected == true
    failed = last_ok + 1

    // Piece is between last_ok and failed coil positions
    // Use moveDumb to push it to the failed coil
    moveDumb(last_ok_pos, failed_pos)

    // Verify it arrived at the recovery target
    if not sensorDetectsPiece(failed_pos):
        return error with diag  // unrecoverable

    // Update piece state to recovered position
    // Recurse to finish the remaining path
    return movePhysicsOrthogonal(failed_pos, dest, max_retry_attempts - 1)
```

### Parameter Threading

- `movePiece(fromX, fromY, toX, toY, max_retry_attempts = 0)` — passes through to `movePhysicsOrthogonal`
- `hexapawnPlay()` calls `movePiece` with `max_retry_attempts = 2` for AI moves
- Human moves in hexapawn don't use physics, so no change needed there
- Frontend `move_piece` and `move_physics` API commands gain an optional `max_retries` param

### Response JSON

The move response gains a `diag` object:

```json
{
  "success": true,
  "error": 0,
  "diag": {
    "checkpoint_ok": true,
    "retries_used": 0,
    "coils": [
      {"sensor": 5, "min": 1682, "detected": true, "arrival": 1695},
      {"sensor": 4, "min": 1710, "detected": true, "arrival": 1720},
      {"sensor": 3, "min": 1690, "detected": true, "arrival": 1685}
    ]
  }
}
```

On failure with diagnostics:

```json
{
  "success": false,
  "error": 6,
  "diag": {
    "checkpoint_ok": false,
    "retries_used": 2,
    "coils": [
      {"sensor": 5, "min": 1682, "detected": true, "arrival": 1695},
      {"sensor": 4, "min": 2018, "detected": false, "arrival": 2020},
      {"sensor": 3, "min": 2025, "detected": false, "arrival": 2022}
    ]
  }
}
```

### Logging

One summary line after each move:

```
physics: diag coil0=OK(1682) coil1=OK(1710) coil2=OK(1690) checkpoint=OK
```

Or on failure:

```
physics: diag coil0=OK(1682) coil1=MISS(2018) coil2=MISS(2025) checkpoint=FAIL
physics: recovery attempt 1: moveDumb to coil1, verified OK
physics: recovery attempt 1: resuming physics move from coil1 to coil2
```

### What Changes

| File | Change |
|------|--------|
| `physics.h` | Add sensor sampling in tick wait loop. Add `MoveDiag` struct. `execute()` returns diag alongside `MoveError`. |
| `board.h` | `movePhysicsOrthogonal()` gains `max_retry_attempts`. Recovery logic. `movePiece()` passes through. `hexapawnPlay()` passes 2. |
| `api.h` | `MoveResponse` gains optional `diag` JSON. `MovePhysicsRequest` / `MovePieceRequest` gain `max_retries`. |
| `serial_server.h` | Parse `max_retries` param in `handleMovePhysics` and `handleMovePiece`. |

### What Doesn't Change

- Physics control loop (force tables, PWM, coasting, centering, jerk limiting)
- `moveDumbOrthogonal()` (used as-is for recovery pushes)
- Hexapawn game logic
- Calibration system
- Frontend widgets (diag data appears in existing log stream)
