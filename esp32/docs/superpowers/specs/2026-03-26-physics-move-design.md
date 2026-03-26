# Physics-Based Piece Movement

## Overview

Replace the current `smartHop`/`moveSmartOrthogonal` implementation with a physics simulation that estimates piece position and velocity continuously, using coil force modeling, friction, and sensor feedback to move pieces accurately across the electromagnetic chess board.

## Data Model

### PieceState (per piece, stored on Board)

```cpp
struct PieceState {
  float x, y;       // estimated position in grid coordinates (0-9 x, 0-6 y)
  float vx, vy;     // velocity in grid-units/sec
  bool stuck;        // true if static friction has not yet been overcome
};
```

### PhysicsParams (sent per-move from frontend, with defaults)

```cpp
struct PhysicsParams {
  // Force model: F = voltage_scale * force_k / (d^falloff_exp + epsilon)
  float force_k;          // coil force constant (tunable)
  float force_epsilon;    // min distance to prevent singularity
  float falloff_exp;      // distance falloff exponent (2.0=inverse square, 3.0=steeper)
  float voltage_scale;    // multiplier for coil strength (1.0 = nominal)

  // Friction
  float friction_static;  // force magnitude threshold to break free
  float friction_kinetic; // viscous damping coefficient (force per unit velocity)

  // Control targets
  float target_velocity;  // desired max speed (grid-units/sec)
  float target_accel;     // max acceleration (grid-units/sec^2)

  // Sensor
  float sensor_threshold; // reading must be this many units below baseline to count

  // Safety
  uint16_t max_duration_ms; // timeout for entire move
};
```

Mass is implicit (folded into `force_k`). All parameters are tunable from the frontend and sent with each move command.

## Coordinate Space

Float grid coordinates in the existing 0-9 x, 0-6 y space. A coil at integer position (3,0) exerts force on a piece at continuous position (2.4, 0.1). Distance is euclidean in grid units. No physical unit conversion needed — constants are tuned empirically.

## Simulation Loop

Runs inline (blocking) inside `movePhysicsOrthogonal`. Sub-millisecond ticks using `micros()` for real delta measurement.

### Per-tick steps:

1. **Measure dt**: `dt = (micros() - last_tick) / 1e6` (in seconds)
2. **Compute coil force**: `F = voltage_scale * force_k / (dist^falloff_exp + epsilon)`, directed from piece toward active coil center
3. **Apply friction**:
   - If `stuck`: check if `|F_coil| > friction_static`. If yes, `stuck = false`. If no, velocity stays zero, skip to step 8.
   - If moving: `F_friction = -friction_kinetic * velocity` (viscous damping)
4. **Net force**: `F_net = F_coil + F_friction`
5. **Clamp acceleration**: if `|F_net| > target_accel`, scale to `target_accel`
6. **Update velocity**: `v += F_net * dt`
7. **Clamp velocity**: if `|v| > target_velocity`, scale to `target_velocity`
8. **Update position**: `pos += v * dt`
9. **Coil switching**: if piece has passed the active coil's center (along movement axis), switch to the next coil in the path. This triggers a full `pulseBit` (SPI write) on the new coil.
10. **Set PWM**: `duty = clamp(|F_desired| / max_force * 255, 0, 255)`. Linear mapping. Skip analogWrite if duty hasn't changed meaningfully (delta < 2).
11. **Sensor correction**: see Sensor Correction section below
12. **Arrival check**: if on last coil AND sensor shows piece centered (reading near `piece_mean`) → done
13. **Timeout check**: if elapsed > `max_duration_ms` → fail with `COIL_FAILURE`

### Tick timing

No fixed tick rate. Each iteration measures actual elapsed time via `micros()`. On ESP32-S3 this naturally runs sub-millisecond. The `sustainCoil` call provides the small delay each tick.

## Coil Switching

- Build path as list of coil positions from source to destination (step by 1 along movement axis)
- Active coil = the next coil ahead of the piece's estimated position
- When estimated position passes the active coil's center, advance to next coil
- Switching triggers `pulseBit()` on new coil (full SPI transaction, clears old bit, sets new)
- Between switches, use `sustainCoil()` (skips SPI, just PWM + delay)

## Sensor Correction

Each sensor covers a 3x3 block centered at `(col*3, row*3)`. When a sensor reading drops below `baseline - sensor_threshold`:

```
reading_ratio = (baseline - reading) / (baseline - piece_mean)
```

- `reading_ratio` ranges from 0.0 (just entered zone) to 1.0 (centered on sensor)
- **Parallel axis (direction of motion)**: estimate position as `sensor_center - (1.0 - reading_ratio) * 1.5 * direction`. Snap the parallel component to this estimate.
- **Perpendicular axis**: keep the simulation estimate unchanged (coil alignment keeps it on track)

Correction only applies when:
- Reading is meaningfully below baseline (exceeds `sensor_threshold`)
- Sensor is in the block the piece is currently estimated to be in, or the next block ahead

## Force-to-PWM Mapping

Linear: `duty = clamp(|F_coil| / max_force * 255, 0, 255)`

Where `max_force = voltage_scale * force_k / epsilon` (force at zero distance). This ensures duty=255 at maximum force and scales down from there.

## Arrival and Stopping

When the piece reaches the final coil in the path:
- Continue pulsing the destination coil until sensor reading stabilizes near `piece_mean`
- The destination coil naturally centers the piece
- If velocity is still positive after sensor shows centered (overshoot), briefly activate the previous coil as a brake via `pulseBit(prevBit, brake_duration, brake_duty)` — this is an optional behavior, enabled by checking velocity sign after arrival

## File Structure

### `physics.h` (new)
- `PieceState` struct
- `PhysicsParams` struct with defaults
- `PhysicsMove` class:
  - Constructor takes `Hardware&` reference and calibration data pointer
  - Single public method: `MoveError execute(PieceState& piece, coil_path, PhysicsParams& params)`
  - All simulation logic is private to this class

### `board.h` (modified)
- Owns `PieceState` per piece (array indexed by piece ID or grid position)
- `movePhysicsOrthogonal(fromX, fromY, toX, toY, PhysicsParams)` — validates move, builds coil path, calls `PhysicsMove::execute()`
- Remove: `SmartParams`, `smartHop`, `moveSmartOrthogonal`, `sensorAt`, `sensorThreshold` (moved to physics.h)

### `hardware.h` (modified)
- Add `sustainCoil(globalBit, duration_us, pwm_duty)` — sustains currently active coil without SPI writes. Validates same bit is active. Updates thermal tracking.
- Existing `pulseBit()` used for coil switches and dumb moves
- Remove `pulseUntilSensor()` — no longer needed

### `api.h` (modified)
- `MovePhysicsRequest` replaces `MoveSmartRequest` — carries all `PhysicsParams` fields plus from/to coordinates
- `MoveSmartRequest` removed

### `serial_server.h` (modified)
- `handleMovePhysics` replaces `handleMoveSmart`
- `move_physics` command replaces `move_smart`

### Frontend
- HexapawnWidget: "Smart" toggle becomes "Physics" toggle
- Tune panel shows `PhysicsParams` fields instead of `SmartParams`

## What Gets Removed

- `SmartParams` struct
- `moveSmartOrthogonal()`
- `smartHop()`
- `pulseUntilSensor()` from Hardware
- `MoveSmartRequest` from API
- `move_smart` command
- All `SMART_*` constants

## Parameters and Defaults

| Parameter | Default | Description |
|-----------|---------|-------------|
| force_k | TBD (tune empirically) | Coil force magnitude constant |
| force_epsilon | 0.5 | Min distance to prevent singularity |
| falloff_exp | 2.0 | Distance falloff exponent |
| voltage_scale | 1.0 | Voltage multiplier for force |
| friction_static | TBD (tune empirically) | Force to overcome stiction |
| friction_kinetic | TBD (tune empirically) | Viscous damping coefficient |
| target_velocity | 3.0 | Max speed in grid-units/sec |
| target_accel | 10.0 | Max acceleration |
| sensor_threshold | 50.0 | ADC units below baseline to trigger |
| max_duration_ms | 5000 | Move timeout |

TBD values will be discovered during initial testing and can be updated via frontend tuning panel.
