# Real-Units Physics Simulation

## Overview

Rework the physics simulation to use real-world units (mm, grams, mN, mm/s) and precomputed force tables instead of the analytical force model. Dynamic friction depends on the vertical force (Fz) from the active coil, which varies with position.

## Units

| Quantity | Unit | Example |
|----------|------|---------|
| Position | mm | piece at (57.0, 38.0) |
| Velocity | mm/s | 100 mm/s cruise speed |
| Acceleration | mm/s² | 500 mm/s² |
| Force | mN | 54 mN lateral, 101 mN vertical |
| Mass | grams | 4.3g piece |
| Time | seconds (dt) | 0.0005s tick |
| Current | amps | 1.0A max |

Conversion: `F(mN) = mass_g * accel_mm_s2 * 1e-3`, or equivalently `accel = F_mN / (mass_g * 1e-3)`.

Gravity in mN: `weight = mass_g * 9.81` (works out because g·m/s² = mN).

## Coordinate Conversion

```
constexpr float GRID_TO_MM = 38.0f / 3.0f;  // 12.667 mm per grid unit
```

Grid position (3,3) → (38.0, 38.0) mm. Coil path built in mm. Force table indexed by offset from coil center in mm.

## PhysicsParams

```cpp
struct PhysicsParams {
  float piece_mass_g       = 4.3f;     // grams (plastic + magnet)
  float max_current_a      = 1.0f;     // max H-bridge current (amps)
  float mu_static          = 0.35f;    // static friction coefficient
  float mu_kinetic         = 0.25f;    // kinetic friction coefficient
  float target_velocity_mm_s = 100.0f; // cruise speed (mm/s)
  float target_accel_mm_s2   = 500.0f; // max acceleration (mm/s²)
  bool active_brake          = true;    // use reverse coil braking
  uint16_t max_duration_ms   = 5000;   // timeout
};
```

8 parameters, all in real units. No force model constants — the force table provides everything.

## Force Computation

Per tick, given piece position (px, py) in mm and active coil center (cx, cy) in mm:

```
dx = px - cx
dy = py - cy

layer = BIT_TO_LAYER[global_bit % 8]
fx_1a = tableForceFx(layer, dx, dy)  // mN at 1A
fy_1a = tableForceFy(layer, dx, dy)  // mN at 1A
fz_1a = tableForceFz(layer, dx, dy)  // mN at 1A (negative = pulls down)
```

## Dynamic Friction

Normal force depends on gravity plus the coil's vertical pull:

```
weight = piece_mass_g * 9.81f                           // mN
normal_force = weight + fz_1a * current_a               // mN (Fz negative adds to weight)
if (normal_force < 0) normal_force = 0                  // piece lifts off
friction_force = mu * max(normal_force, 0)              // mN
```

When coil is active at center of closest layer: normal = 42.2 + 101 = 143 mN, friction = 0.25 * 143 = 35.8 mN. Peak lateral force is 54 mN. Net lateral: ~18 mN. This explains why movement is sluggish — the coil's own downward pull fights its lateral pull.

## Controller

Same structure as current — compute desired force, map to duty:

```
// Velocity error along movement axis
v_along = project velocity onto movement direction
speed_error = target_velocity_mm_s - v_along
desired_accel = clamp(speed_error / dt, -target_accel, +target_accel)

// Desired force = mass * accel + friction compensation
desired_lateral = piece_mass_g * 1e-3f * desired_accel + friction_force  // mN

// Map to current via force table
available_lateral = sqrt(fx_1a² + fy_1a²)  // mN at 1A
required_current = desired_lateral / max(available_lateral, 0.01)
duty = clamp(required_current / max_current_a * 255, 0, 255)

// Actual force
actual_current = (duty / 255.0f) * max_current_a
fx = fx_1a * actual_current
fy = fy_1a * actual_current

// Acceleration
ax = fx / (piece_mass_g * 1e-3f)  // mm/s²
ay = fy / (piece_mass_g * 1e-3f)
```

Note: friction in the controller uses `mu_kinetic` when moving, `mu_static` for the stuck check. The Fz for friction uses the actual current being applied (circular dependency resolved by using previous tick's current).

## Layer Mapping

```cpp
constexpr int BIT_TO_LAYER[] = {2, 1, 0, 3, 4};  // local_bit → layer index

static int bitToLayer(uint8_t global_bit) {
  uint8_t local = global_bit % 8;
  return (local < 5) ? BIT_TO_LAYER[local] : 0;
}
```

## Coil Path in mm

`movePhysicsOrthogonal` converts grid path to mm:

```cpp
constexpr float GRID_TO_MM = 38.0f / 3.0f;

// When building path:
path_mm[i][0] = grid_x * GRID_TO_MM;
path_mm[i][1] = grid_y * GRID_TO_MM;
```

Coil switching threshold: piece passes coil center along movement axis (same logic, now in mm).

## PieceState

```cpp
struct PieceState {
  float x, y;     // mm
  float vx, vy;   // mm/s
  bool stuck;
};
```

## Stopping Strategy

Each tick, the controller checks whether it's time to stop:

```
// Friction-only deceleration (coil off, just gravity for normal force)
friction_decel = mu_kinetic * piece_mass_g * 9.81f / (piece_mass_g * 1e-3f)  // mm/s²

if (active_brake):
  // Lookup brake force from previous coil pulling backwards
  brake_force = table_lookup(prev_coil, piece_pos)  // mN at max_current
  brake_decel = friction_decel + brake_force / (piece_mass_g * 1e-3f)
  brake_decel = min(brake_decel, target_accel_mm_s2)  // cap to max accel
else:
  brake_decel = friction_decel

stopping_dist = v² / (2 * brake_decel)

if (stopping_dist >= dist_remaining):
  // Time to stop
  stop current coil
  if (active_brake):
    activate previous coil at duty proportional to desired decel
  // Simulation continues with no forward coil, friction/brake decelerates
```

**Passive stop**: Cut the coil, friction coasts piece to rest. Simple, slightly less precise.

**Active brake** (default): Cut forward coil, briefly activate previous coil to pull backwards. Capped by `target_accel_mm_s2` to prevent slamming. Allows higher cruise speed since braking distance is shorter.

After the coil is cut, the simulation continues ticking — friction (and optional brake) decelerates the piece. Arrival is detected when the piece is close and slow enough.

## Arrival Check

Position-based: `distance_to_destination < 1.0mm && speed < 5.0mm/s`. These thresholds are in mm now.

## Static Friction Check

```
available_force = sqrt(fx_1a² + fy_1a²) * max_current_a  // mN at full current
weight = piece_mass_g * 9.81f
normal = weight + fz_1a * max_current_a
static_friction = mu_static * max(normal, 0)
can_move = available_force > static_friction
```

## File Changes

### `firmware/physics.h`
- New `PhysicsParams` with real-unit fields
- `GRID_TO_MM` constant
- `bitToLayer()` helper
- `execute()` rewritten with table-based force, dynamic friction, mm units
- Remove `coilForce()`, `maxForce()`, `forceToDuty()`
- Keep `tableForceFx/Fy/Fz`, `coordToBit`, `startCoil/stopCoil/sustainCoil`

### `firmware/board.h`
- `movePhysicsOrthogonal` builds path in mm (multiply by GRID_TO_MM)
- `PieceState` positions in mm

### `firmware/api.h`
- `MovePhysicsRequest` new fields matching PhysicsParams

### `firmware/serial_server.h`
- `parsePhysicsParams` updated for new field names

### `frontend/src/widgets/HexapawnWidget.tsx`
- `DEFAULT_PHYSICS_PARAMS` with real-unit defaults
- `PARAM_INFO` with proper unit labels
- `PhysicsDebug` updated for real-unit derived values

### `frontend/src/widgets/MoveTestWidget.tsx`
- Same param updates

### `tests/test_physics.cpp`
- Updated for mm units, force table stubs

## What Gets Removed

- `coilForce()`, `maxForce()`, `forceToDuty()` from PhysicsMove
- `force_k`, `force_epsilon`, `falloff_exp`, `voltage_scale` from PhysicsParams
- `friction_static`, `friction_kinetic` (replaced by `mu_static`, `mu_kinetic`)
- `target_velocity`, `target_accel` (replaced by `_mm_s` / `_mm_s2` variants)
- Analytical force model entirely
