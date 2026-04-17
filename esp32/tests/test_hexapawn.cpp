// Local test + profile harness for the hexapawn perfect-play solver.
//
// Build: g++ -std=c++17 -O2 -I../firmware -o test_hexapawn test_hexapawn.cpp
// Run:   ./test_hexapawn

#include "hexapawn.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
  if (cond) { g_passed++; } \
  else { g_failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static const char* cellStr(int8_t v) {
  if (v == HP_WHITE) return "P";
  if (v == HP_BLACK) return "p";
  return ".";
}

static void printBoard(const Hexapawn& g) {
  for (int r = HP_SIZE - 1; r >= 0; r--) {
    fprintf(stderr, "  ");
    for (int c = 0; c < HP_SIZE; c++) fprintf(stderr, "%s", cellStr(g.board[c][r]));
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "  turn=%s  winner=%d\n",
          g.turn == HP_WHITE ? "White" : "Black", g.winner);
}

// ── Basic mechanical tests ───────────────────────────────────────

static void test_reset_and_initial_moves() {
  Hexapawn g; g.reset();
  CHECK(g.turn == HP_WHITE, "initial turn is white");
  CHECK(g.winner == HP_NONE, "no winner at start");

  HexapawnMove m[18];
  int n = g.getValidMoves(HP_WHITE, m);
  CHECK(n == 3, "white has 3 initial moves (one forward from each col)");

  n = g.getValidMoves(HP_BLACK, m);
  CHECK(n == 3, "black has 3 initial moves");
}

static void test_apply_and_capture() {
  Hexapawn g; g.reset();

  // Advance white (1,0)->(1,1): now facing all 3 black pawns
  HexapawnMove w1 = {1, 0, 1, 1};
  CHECK(g.isValidMove(1, 0, 1, 1), "(1,0)->(1,1) valid");
  g.applyMove(w1);

  // Black captures diagonally: (0,2)->(1,1)
  CHECK(g.turn == HP_BLACK, "black to move");
  CHECK(g.isValidMove(0, 2, 1, 1), "black (0,2)->(1,1) capture valid");
  HexapawnMove b1 = {0, 2, 1, 1};
  g.applyMove(b1);
  CHECK(g.board[1][1] == HP_BLACK, "black occupies (1,1) after capture");
  CHECK(g.board[0][2] == HP_NONE, "(0,2) now empty");
}

static void test_terminal_reaches_last_rank() {
  Hexapawn g; g.reset();
  // Hand-crafted terminal: white pawn at (0,2) should be a win.
  memset(g.board, HP_NONE, sizeof(g.board));
  g.board[0][2] = HP_WHITE;
  g.turn = HP_BLACK;
  g.checkWin();
  CHECK(g.winner == HP_WHITE, "white reaching last rank wins");
}

static void test_terminal_stalemate() {
  Hexapawn g; g.reset();
  // Black to move but has no legal move. Single black pawn at (0,2) with
  // white blocking (0,1) forward; (1,1) empty so no diagonal capture.
  memset(g.board, HP_NONE, sizeof(g.board));
  g.board[0][2] = HP_BLACK;
  g.board[0][1] = HP_WHITE;  // blocks black forward
  g.board[2][0] = HP_WHITE;  // gives white something to move, so white isn't stalemated
  g.turn = HP_BLACK;
  g.checkWin();
  CHECK(g.winner == HP_WHITE, "black stalemated → white wins");
}

// ── Solver correctness ───────────────────────────────────────────
//
// Hexapawn with optimal play is a win for the SECOND player (Black here):
// see e.g. Martin Gardner's 1962 Scientific American column.

static void test_perfect_play_black_wins() {
  Hexapawn g; g.reset();
  int initial_score = g.optimalScore();
  // Initial side-to-move is White. Full-game score from white's POV
  // should be -100 (losing) since black wins with perfect play.
  CHECK(initial_score == -100,
        "initial position is a loss for white (first player)");
}

static void test_perfect_vs_perfect_is_deterministic() {
  Hexapawn g; g.reset();
  int moves = 0;
  while (g.winner == HP_NONE && moves < 20) {
    HexapawnMove m = g.computeAiMove();
    g.applyMove(m);
    moves++;
  }
  CHECK(g.winner == HP_BLACK, "perfect-vs-perfect: black wins");
  CHECK(moves <= 10, "perfect-vs-perfect terminates quickly");
}

static void test_perfect_never_loses_to_random(std::mt19937& rng) {
  // Black plays perfectly, white plays randomly. Black should never lose.
  int losses = 0;
  int games = 300;
  for (int i = 0; i < games; i++) {
    Hexapawn g; g.reset();
    while (g.winner == HP_NONE) {
      if (g.turn == HP_BLACK) {
        HexapawnMove m = g.computeAiMove();
        g.applyMove(m);
      } else {
        HexapawnMove moves[18];
        int n = g.getValidMoves(HP_WHITE, moves);
        if (n == 0) break;
        g.applyMove(moves[rng() % n]);
      }
    }
    if (g.winner == HP_WHITE) losses++;
  }
  CHECK(losses == 0, "perfect black never loses to random white");
  printf("  perfect black vs random white: %d games, %d losses\n", games, losses);
}

static void test_random_vs_perfect(std::mt19937& rng) {
  // White plays perfectly, black plays randomly. Per hexapawn theory,
  // perfect play here is still a LOSS for white (the first player) against
  // an optimal opponent — but against random play it should usually win.
  int wins = 0;
  int games = 300;
  for (int i = 0; i < games; i++) {
    Hexapawn g; g.reset();
    while (g.winner == HP_NONE) {
      if (g.turn == HP_WHITE) {
        HexapawnMove m = g.computeAiMove();
        g.applyMove(m);
      } else {
        HexapawnMove moves[18];
        int n = g.getValidMoves(HP_BLACK, moves);
        if (n == 0) break;
        g.applyMove(moves[rng() % n]);
      }
    }
    if (g.winner == HP_WHITE) wins++;
  }
  printf("  perfect white vs random black: %d games, %d white wins (%.0f%%)\n",
         games, wins, 100.0 * wins / games);
  // Hexapawn is a loss for white with optimal play on both sides, so even
  // "perfect" white can't guarantee wins — but it should still win some
  // games when black errs randomly.
  CHECK(wins > 0, "perfect white wins some games against random black");
}

// ── Profiling ────────────────────────────────────────────────────

static void profile_solver() {
  Hexapawn g; g.reset();

  // Cold: first call builds the memoization table from scratch.
  auto t0 = std::chrono::high_resolution_clock::now();
  HexapawnMove m = g.computeAiMove();
  auto t1 = std::chrono::high_resolution_clock::now();
  double cold_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  (void)m;
  printf("  cold computeAiMove (start position): %8.1f us\n", cold_us);

  // Warm: memo is populated. Measure across many calls at varied positions.
  std::vector<double> times;
  std::mt19937 rng(12345);
  const int N = 5000;
  times.reserve(N);

  for (int i = 0; i < N; i++) {
    Hexapawn h; h.reset();
    // Play 0–3 random half-moves to reach a varied position.
    int plies = rng() % 4;
    for (int p = 0; p < plies && h.winner == HP_NONE; p++) {
      HexapawnMove moves[18];
      int n = h.getValidMoves(h.turn, moves);
      if (n == 0) break;
      h.applyMove(moves[rng() % n]);
    }
    if (h.winner != HP_NONE) continue;

    auto ta = std::chrono::high_resolution_clock::now();
    HexapawnMove mm = h.computeAiMove();
    auto tb = std::chrono::high_resolution_clock::now();
    (void)mm;
    times.push_back(std::chrono::duration<double, std::micro>(tb - ta).count());
  }

  std::sort(times.begin(), times.end());
  auto pct = [&](double p) { return times[(size_t)(p * (times.size() - 1))]; };
  double sum = 0;
  for (double v : times) sum += v;
  printf("  warm computeAiMove (%zu samples from random positions):\n", times.size());
  printf("    mean:  %8.2f us\n", sum / times.size());
  printf("    p50:   %8.2f us\n", pct(0.50));
  printf("    p90:   %8.2f us\n", pct(0.90));
  printf("    p99:   %8.2f us\n", pct(0.99));
  printf("    max:   %8.2f us\n", times.back());
}

int main() {
  printf("=== hexapawn tests ===\n");
  test_reset_and_initial_moves();
  test_apply_and_capture();
  test_terminal_reaches_last_rank();
  test_terminal_stalemate();
  test_perfect_play_black_wins();
  test_perfect_vs_perfect_is_deterministic();

  std::mt19937 rng(42);
  test_perfect_never_loses_to_random(rng);
  test_random_vs_perfect(rng);

  printf("  %d passed, %d failed\n", g_passed, g_failed);

  printf("\n=== hexapawn profile ===\n");
  profile_solver();

  return g_failed == 0 ? 0 : 1;
}
