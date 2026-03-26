# Physics Auto-Tuning System

## Overview

An automated tuning routine that discovers optimal PhysicsParams by measuring the real board's sensor response, coil strength, and friction characteristics. Runs as a single blocking serial command (`tune_physics`), streams progress via LOG_BOARD, and returns a complete recommended PhysicsParams set that the frontend can one-click apply.

## Prerequisites

- A single magnetic piece, placed at (3,3) by the user before starting
- Calibration data is helpful (provides baseline/piece_mean) but not required (uses manual_baseline/manual_piece_mean as fallback)

## Tuning Phases

### Phase 1: Sensor Curve Fitting

**Goal:** Discover `sensor_k` and `sensor_falloff` by measuring sensor readings at known distances.

**Test position:** Sensor s4 at (3,3) (sr_col=1, sr_row=1).

**Procedure:**
1. Read sensor s4 with piece at (3,3) — distance 0. Collect 5 readings.
2. Dumb-move piece to (4,3) — distance 1. Read sensor s4 5 times. Dumb-move back to (3,3).
3. Dumb-move piece to (5,3) — distance 2. Read sensor s4 5 times. Dumb-move back to (3,3).

**Fitting:**
The sensor model is `strength = sensor_k / (d^sensor_falloff + epsilon)` where `strength = baseline - reading`.

With medians at d=0, d=1, d=2:
- `sensor_k = strength_0 * epsilon` (from d=0 where `d^falloff = 0`)
- Ratio `strength_0 / strength_1 = (1 + epsilon) / epsilon` constrains epsilon confirmation
- Ratio `strength_1 / strength_2 = (2^falloff + epsilon) / (1 + epsilon)` gives `sensor_falloff`

Simple algebraic solve, no iterative optimization.

### Phase 2: Static Friction Measurement

**Goal:** Discover `friction_static` — the minimum force needed to start the piece moving.

**Procedure (per rep, 5 reps):**
1. Piece centered at (3,3). Read sensor s4 as baseline.
2. Pulse coil at (4,3) (1 grid unit away) at duty D for 100ms.
3. Read sensor s4. If reading changed by >20 ADC from baseline, piece moved.
4. Dumb-move piece back to (3,3) to re-center.
5. Sweep duty from 10 to 255 in steps of 10. Record lowest duty that causes movement.

**5 reps.** Take median of threshold duties.

**Converting to friction_static:**
```
friction_static = coilForce(1.0, params) * (median_duty / 255.0)
```
Uses the force model at d=1.0 (distance from piece to target coil). `force_k` uses default until Phase 3 refines it — static friction is a ratio, so approximate force_k is acceptable.

### Phase 3: Coil Force Profiling

**Goal:** Discover `force_k` and `friction_kinetic` by measuring displacement for known pulse durations.

**Procedure (per duration, 5 reps each):**
1. Piece centered at (3,3). Read sensor s4 baseline.
2. Pulse coil (4,3) at duty 255 for duration D.
3. Wait 50ms settle.
4. Read sensor s4. Use fitted sensor model (from Phase 1) to convert reading change to displacement.
5. Dumb-move piece back to (3,3).

**Durations:** 10ms, 25ms, 50ms, 100ms, 200ms.

**Fitting force_k:**
Knowing `friction_static` (Phase 2), the piece accelerates at `F_coil - friction` once unstuck. Displacement after pulse relates to force magnitude. Fit `force_k` to match observed displacements.

**Fitting friction_kinetic:**
After the coil turns off, the piece decelerates at `friction_kinetic * v`. The ratio between displacements at different durations constrains kinetic friction — longer pulses show diminishing returns as friction limits top speed.

Two-parameter fit (force_k, friction_kinetic) from 5 displacement data points (medians of 5 reps each).

### Phase 4: Move Verification

**Goal:** Validate discovered params with real physics moves.

**Procedure (5 reps):**
1. Dumb-move piece to (5,3) — 2 grid units from test center.
2. Attempt `movePhysicsOrthogonal(5, 3, 3, 3)` with discovered params.
3. Wait 100ms settle.
4. Read sensor s4 to check arrival (reading near piece_mean = success).
5. Read all sensors to find actual piece location (lowest reading).
6. Record: success/fail, destination reading, actual sensor, elapsed time.
7. Dumb-move piece back to (3,3).

Reports success rate (out of 5) and average position accuracy.

## Architecture

### File: `board.h` — `tunePhysics()` method

Single public method replacing the current simple `tunePhysics`. Runs all 4 phases, streams logs, returns JSON with results and recommended params.

Uses existing infrastructure:
- `calMove()` for dumb-move repositioning (with skipValidation)
- `hw_.pulseBit()` for controlled pulses during friction/force measurement
- `hw_.readSensor()` for all sensor readings
- `movePhysicsOrthogonal()` for verification moves
- `coordToBit()` / `sensorForGrid()` for grid-to-hardware mapping

### Return Format

```json
{
  "sensor_fit": {
    "sensor_k": 350.0,
    "sensor_falloff": 1.8,
    "readings_d0": [1700, 1705, 1698, 1702, 1701],
    "readings_d1": [1850, 1845, 1852, 1848, 1847],
    "readings_d2": [1990, 1995, 1988, 1992, 1991]
  },
  "static_friction": {
    "threshold_duties": [40, 50, 40, 50, 40],
    "median_duty": 40,
    "friction_static": 2.1
  },
  "force_profile": {
    "durations_ms": [10, 25, 50, 100, 200],
    "displacements": [0.1, 0.3, 0.6, 1.0, 1.4],
    "force_k": 8.5,
    "friction_kinetic": 1.8
  },
  "verification": {
    "success_count": 4,
    "total": 5,
    "dest_readings": [1720, 1715, 1730, 1710, 1850],
    "elapsed_ms": [890, 920, 880, 910, 5000]
  },
  "recommended": {
    "force_k": 8.5,
    "force_epsilon": 0.3,
    "falloff_exp": 2.0,
    "voltage_scale": 1.0,
    "friction_static": 2.1,
    "friction_kinetic": 1.8,
    "target_velocity": 5.0,
    "target_accel": 20.0,
    "sensor_k": 350.0,
    "sensor_falloff": 1.8,
    "sensor_threshold": 50.0,
    "manual_baseline": 2030.0,
    "manual_piece_mean": 1700.0,
    "max_duration_ms": 5000
  }
}
```

The `recommended` block is a complete PhysicsParams. Values not measured (force_epsilon, falloff_exp, voltage_scale, target_velocity, target_accel, sensor_threshold, manual_baseline, manual_piece_mean, max_duration_ms) keep their compiled defaults.

### API

- Command: `tune_physics` (POST)
- Request: `TunePhysicsRequest {}` (no params — uses defaults and calibration data)
- Response: The JSON above, returned as raw string (like `GetCalibrationResponse`)
- Streams progress via LOG_BOARD with `TUNE:` prefix

### Frontend

The CalibrationWidget gets a "Tune Physics" button (alongside "Start Calibration"). When tuning completes:
- Shows the verification results (success rate, timing)
- Shows a "Apply Recommended" button that copies the `recommended` params into the HexapawnWidget's physics params state
- Download JSON button for the full results

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| TUNE_NUM_REPS | 5 | Repetitions per measurement |
| TUNE_SENSOR_READ_COUNT | 5 | Sensor readings per distance point |
| TUNE_FRICTION_PULSE_MS | 100 | Pulse duration for friction test |
| TUNE_FRICTION_DUTY_STEP | 10 | Duty increment per friction sweep step |
| TUNE_SETTLE_MS | 50 | Wait after pulse before reading sensor |
| TUNE_TEST_X | 3 | Test center X position |
| TUNE_TEST_Y | 3 | Test center Y position |

### Estimated Duration

- Phase 1: 3 positions * 5 readings + dumb moves ≈ 15s
- Phase 2: 5 reps * 25 duty steps * 100ms pulse + re-centering ≈ 90s
- Phase 3: 5 durations * 5 reps * (pulse + settle + re-center) ≈ 60s
- Phase 4: 5 reps * (dumb move + physics move + settle) ≈ 30s
- **Total: ~3-4 minutes**

## What Gets Removed

The current `tunePhysics()` method in board.h (simple 4-direction trial) is replaced entirely.

## What Stays

- All existing PhysicsParams, PhysicsMove, physics.h unchanged
- Calibration system unchanged
- moveDumbOrthogonal, calMove unchanged (used heavily by tuning)
- Frontend physics toggle and tune panel unchanged (tuning just provides better default values)
