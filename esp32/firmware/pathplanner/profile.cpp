// Profile the path planner on randomized hexapawn-like states.
//
// Generates N random 4x3 board configurations with 3 white + 3 black
// pieces arranged in the play area (cols 0-2, rows 0-2), with a random
// 1- or 2-piece commanded move (second move simulates capture →
// graveyard at col 3).
//
// Build: g++ -std=c++17 -O2 -DPP_HOST_LOG=0 -o profile profile.cpp pathplanner.cpp
// Run:   ./profile [N]

#include "../pathplanner.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <random>

static constexpr int COLS = 4;
static constexpr int ROWS = 3;
static constexpr int GRAVE_COL = 3;

struct Case {
  uint8_t board[PP_MAX_COLS][PP_MAX_ROWS];
  MoveGoal goals[4];
  int num_goals;
};

// Generate a random hexapawn-style state:
//  - 3 white pawns somewhere in cols 0..2 (not all on same row)
//  - 3 black pawns somewhere in cols 0..2 (no overlap with white)
//  - One commanded move: pick a random piece, move it one cell
//    orthogonally to an empty cell in the play area. 30% of the time,
//    also emit a capture: the destination is an enemy cell, and that
//    enemy is sent to the graveyard (col 3).
static Case gen_case(std::mt19937& rng) {
  Case c;
  memset(c.board, PP_NONE, sizeof(c.board));

  // Pick 6 distinct cells in the 3x3 play area, first 3 white, next 3 black.
  std::vector<std::pair<int,int>> cells;
  for (int x = 0; x < 3; x++)
    for (int y = 0; y < ROWS; y++)
      cells.push_back({x, y});
  std::shuffle(cells.begin(), cells.end(), rng);

  std::pair<int,int> w[3], b[3];
  for (int i = 0; i < 3; i++) { w[i] = cells[i];     c.board[w[i].first][w[i].second] = PP_WHITE; }
  for (int i = 0; i < 3; i++) { b[i] = cells[3 + i]; c.board[b[i].first][b[i].second] = PP_BLACK; }

  // Pick a piece to move (random white or black).
  bool use_white = (rng() & 1);
  auto& mover = use_white ? w[rng() % 3] : b[rng() % 3];

  // Find a candidate destination: adjacent cell in play area.
  const int8_t DX[4] = {1, -1, 0, 0};
  const int8_t DY[4] = {0, 0, 1, -1};
  int order[4] = {0, 1, 2, 3};
  std::shuffle(order, order + 4, rng);

  int dx = 0, dy = 0;
  int tx = -1, ty = -1;
  bool capture = false;
  for (int k = 0; k < 4; k++) {
    dx = DX[order[k]];
    dy = DY[order[k]];
    int nx = mover.first + dx;
    int ny = mover.second + dy;
    if (nx < 0 || nx >= 3 || ny < 0 || ny >= ROWS) continue;
    // 30% chance: if occupied by enemy, treat as capture.
    if (c.board[nx][ny] != PP_NONE) {
      uint8_t occ = c.board[nx][ny];
      uint8_t self = c.board[mover.first][mover.second];
      if (occ != self && (rng() % 10) < 3) {
        tx = nx; ty = ny; capture = true; break;
      }
      continue;
    }
    tx = nx; ty = ny; break;
  }

  if (tx < 0) {
    // No valid move — fall back to trivial: skip this case by re-rolling.
    return gen_case(rng);
  }

  c.num_goals = capture ? 2 : 1;
  c.goals[0] = {(uint8_t)mover.first, (uint8_t)mover.second,
                (uint8_t)tx, (uint8_t)ty};
  if (capture) {
    // Captured piece goes to graveyard (col 3), random row.
    int gy = rng() % ROWS;
    c.goals[1] = {(uint8_t)tx, (uint8_t)ty,
                  (uint8_t)GRAVE_COL, (uint8_t)gy};
  }
  return c;
}

int main(int argc, char** argv) {
  int N = (argc > 1) ? atoi(argv[1]) : 1000;
  uint64_t seed = (argc > 2) ? strtoull(argv[2], nullptr, 10) : 42;
  uint8_t max_concurrent = (argc > 3) ? (uint8_t)atoi(argv[3]) : 0;
  std::mt19937 rng(seed);

  std::vector<double> times_us;
  times_us.reserve(N);
  int ok = 0, nopath = 0, other = 0;
  int capture_cases = 0;
  int slow_count = 0;
  double total_us = 0;
  double max_us = 0;
  constexpr double SLOW_US = 10000.0;  // 10 ms — flag slow cases

  auto dump_case = [](const Case& c) {
    fprintf(stderr, "  board:\n");
    for (int y = ROWS - 1; y >= 0; y--) {
      fprintf(stderr, "    ");
      for (int x = 0; x < COLS; x++) {
        uint8_t p = c.board[x][y];
        fputc(p == PP_WHITE ? 'P' : p == PP_BLACK ? 'p' : '.', stderr);
      }
      fputc('\n', stderr);
    }
    for (int i = 0; i < c.num_goals; i++)
      fprintf(stderr, "  goal: (%d,%d) -> (%d,%d)\n",
              c.goals[i].fromX, c.goals[i].fromY,
              c.goals[i].toX, c.goals[i].toY);
  };

  for (int i = 0; i < N; i++) {
    Case c = gen_case(rng);
    if (c.num_goals == 2) capture_cases++;

    uint8_t board_copy[PP_MAX_COLS][PP_MAX_ROWS];
    memcpy(board_copy, c.board, sizeof(board_copy));

    auto t0 = std::chrono::high_resolution_clock::now();
    PlanResult r = planPath(board_copy, COLS, ROWS, c.goals, c.num_goals,
                            max_concurrent);
    auto t1 = std::chrono::high_resolution_clock::now();

    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    times_us.push_back(us);
    total_us += us;
    if (us > max_us) max_us = us;

    if (us > SLOW_US) {
      slow_count++;
      fprintf(stderr, "[slow] case %d: %.1f ms, status=%d\n", i, us / 1000.0, r.status);
      dump_case(c);
    }

    if (r.status == PlanResult::OK) ok++;
    else if (r.status == PlanResult::NO_PATH) nopath++;
    else other++;
  }

  std::sort(times_us.begin(), times_us.end());
  auto pct = [&](double p) {
    int i = (int)(p * (N - 1));
    return times_us[i];
  };

  printf("cases: %d  max_concurrent=%u  (captures: %d, %.0f%%)\n",
         N, max_concurrent, capture_cases, 100.0 * capture_cases / N);
  printf("  OK: %d   NO_PATH: %d   other: %d\n", ok, nopath, other);
  printf("  mean:  %8.1f us\n", total_us / N);
  printf("  p50:   %8.1f us\n", pct(0.50));
  printf("  p90:   %8.1f us\n", pct(0.90));
  printf("  p99:   %8.1f us\n", pct(0.99));
  printf("  max:   %8.1f us\n", max_us);
  printf("  slow (>%.0fms): %d\n", SLOW_US / 1000.0, slow_count);
  return 0;
}
