// Path planner for orthogonal piece movement on the coil grid.
// Usable from both firmware (Board) and local test harness.

#pragma once
#include <cstdint>
#include <cstddef>

constexpr uint8_t PP_MAX_COLS = 12;
constexpr uint8_t PP_MAX_ROWS = 8;
constexpr int PP_MAX_GOALS = 12;   // includes implicit obstacle goals
constexpr int PP_MAX_TIMESTEPS = 32;

#define PP_NONE  0
#define PP_WHITE 1
#define PP_BLACK 2

constexpr uint8_t PP_ANY = 255;  // wildcard: planner picks best value

struct MoveGoal {
  uint8_t fromX, fromY;
  uint8_t toX, toY;  // PP_ANY = planner chooses optimal value for that axis
};

// One piece's movement within a timestep
struct PieceMove {
  uint8_t fromX, fromY;
  uint8_t toX, toY;
};

// A timestep: all pieces that move this tick (simultaneously)
struct TimeStep {
  PieceMove moves[PP_MAX_GOALS];
  int num_moves;  // how many pieces moved this tick
};

struct PlanResult {
  enum Status : uint8_t { OK, NO_PIECE, OUT_OF_BOUNDS, NO_PATH };
  Status status;
  TimeStep steps[PP_MAX_TIMESTEPS];
  int num_steps;  // number of timesteps
};

// Plan simultaneous movement of multiple pieces. Each timestep, any
// subset of pieces may move one cell orthogonally. Finds the solution
// with the fewest timesteps, subject to an optional per-tick cap on
// how many pieces may move at once (to bound peak power draw).
//
// board: cols x rows grid, board[x][y] = piece ID (0=empty)
// max_concurrent_moves: hard cap on movers per timestep. Counts both
//     commanded and implicit (obstacle) motion. 0 = unlimited.
// The planner mutates `board` to reflect the final state.
PlanResult planPath(uint8_t board[][PP_MAX_ROWS],
                    uint8_t cols, uint8_t rows,
                    const MoveGoal* goals, int num_goals,
                    uint8_t max_concurrent_moves = 0);
