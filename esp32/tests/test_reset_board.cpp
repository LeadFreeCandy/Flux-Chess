// Local test for resetBoard's goal assignment + path planning.
//
// Reproduces the end-of-game state from the user's bug report:
//   Whites: on board at (1,0) and (2,1) + one in graveyard at (3,1)
//   Blacks: on board at (1,1) and (2,2) + one in graveyard at (3,0)
//
// Verifies:
//   1. When all 6 pieces are tracked, the greedy assignment emits 6 goals
//      and the planner solves them.
//   2. When the graveyard white is missing from `pieces_` (the actual bug),
//      the greedy emits only 5 goals AND the planner can't solve them.
//
// Build: g++ -std=c++17 -O2 -I../firmware -o test_reset_board \
//        test_reset_board.cpp ../firmware/pathplanner.cpp
// Run:   ./test_reset_board

#include "pathplanner.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
  if (cond) { g_passed++; printf("  PASS: %s\n", msg); } \
  else { g_failed++; printf("  FAIL: %s\n", msg); } \
} while (0)

static constexpr uint8_t MAIN_COLS = 4;
static constexpr uint8_t MAIN_ROWS = 3;

// Mirrors Board::resetBoard's greedy assignment.
struct Goal { uint8_t fx, fy, tx, ty; const char* color; };
struct Pos { uint8_t x, y; };

static int assignColor(const Pos* pieces, int n,
                       const uint8_t targets[][2], int nt,
                       Goal* out, int outStart, const char* colorName) {
  bool assigned[12] = {};
  int count = 0;
  for (int t = 0; t < nt; t++) {
    int best = -1, bestDist = 999;
    for (int i = 0; i < n; i++) {
      if (assigned[i]) continue;
      int d = abs((int)pieces[i].x - (int)targets[t][0]) +
              abs((int)pieces[i].y - (int)targets[t][1]);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best < 0) break;
    assigned[best] = true;
    if (pieces[best].x != targets[t][0] || pieces[best].y != targets[t][1]) {
      out[outStart + count] = { pieces[best].x, pieces[best].y,
                                targets[t][0], targets[t][1], colorName };
      count++;
    }
  }
  return count;
}

// Returns number of goals generated.
static int runAssignment(const uint8_t board[MAIN_COLS][MAIN_ROWS], Goal* out) {
  static const uint8_t W_TARGETS[3][2] = {{0, 0}, {1, 0}, {2, 0}};
  static const uint8_t B_TARGETS[3][2] = {{0, 2}, {1, 2}, {2, 2}};

  Pos whites[12], blacks[12];
  int nw = 0, nb = 0;
  for (int mx = 0; mx < MAIN_COLS; mx++) {
    for (int my = 0; my < MAIN_ROWS; my++) {
      if (board[mx][my] == PP_WHITE) whites[nw++] = {(uint8_t)mx, (uint8_t)my};
      else if (board[mx][my] == PP_BLACK) blacks[nb++] = {(uint8_t)mx, (uint8_t)my};
    }
  }

  int ng = 0;
  ng += assignColor(whites, nw, W_TARGETS, 3, out, ng, "white");
  ng += assignColor(blacks, nb, B_TARGETS, 3, out, ng, "black");
  return ng;
}

static void printBoard(const uint8_t board[MAIN_COLS][MAIN_ROWS]) {
  for (int y = MAIN_ROWS - 1; y >= 0; y--) {
    printf("    ");
    for (int x = 0; x < MAIN_COLS; x++) {
      char c = '.';
      if (board[x][y] == PP_WHITE) c = 'P';
      else if (board[x][y] == PP_BLACK) c = 'p';
      printf("%c ", c);
    }
    printf("\n");
  }
}

static void buildEndOfGameBoard(uint8_t board[MAIN_COLS][MAIN_ROWS],
                                 bool trackGraveyardWhite) {
  memset(board, PP_NONE, MAIN_COLS * MAIN_ROWS);
  // Whites on board
  board[1][0] = PP_WHITE;
  board[2][1] = PP_WHITE;
  // Blacks on board
  board[1][1] = PP_BLACK;
  board[2][2] = PP_BLACK;
  // Graveyard
  board[3][0] = PP_BLACK;              // always tracked (from human capture)
  if (trackGraveyardWhite) {
    board[3][1] = PP_WHITE;            // from AI capture — the suspect one
  }
}

static bool runPlan(uint8_t board[MAIN_COLS][MAIN_ROWS],
                    Goal* goals, int ng, PlanResult& out) {
  // Need the full [PP_MAX_COLS][PP_MAX_ROWS] shape for planPath.
  uint8_t wide[PP_MAX_COLS][PP_MAX_ROWS];
  memset(wide, PP_NONE, sizeof(wide));
  for (int x = 0; x < MAIN_COLS; x++)
    for (int y = 0; y < MAIN_ROWS; y++) wide[x][y] = board[x][y];

  MoveGoal pgoals[PP_MAX_GOALS];
  for (int i = 0; i < ng; i++) {
    pgoals[i] = { goals[i].fx, goals[i].fy, goals[i].tx, goals[i].ty };
  }

  out = planPath(wide, MAIN_COLS, MAIN_ROWS, pgoals, ng);
  return out.status == PlanResult::OK;
}

static void test_all_tracked() {
  printf("\n=== TEST: all 6 pieces tracked (expected behaviour) ===\n");
  uint8_t board[MAIN_COLS][MAIN_ROWS];
  buildEndOfGameBoard(board, /*trackGraveyardWhite=*/true);
  printBoard(board);

  Goal goals[PP_MAX_GOALS];
  int ng = runAssignment(board, goals);
  printf("  Assigned %d goals:\n", ng);
  for (int i = 0; i < ng; i++) {
    printf("    %s (%d,%d) -> (%d,%d)\n",
           goals[i].color, goals[i].fx, goals[i].fy, goals[i].tx, goals[i].ty);
  }

  CHECK(ng == 6, "all 6 pieces produce 6 goals (3 per colour)");

  PlanResult plan;
  bool ok = runPlan(board, goals, ng, plan);
  CHECK(ok, "planPath solves the 6-goal configuration");
  printf("  -> plan status=%d, num_steps=%d\n", (int)plan.status, plan.num_steps);
}

static void test_graveyard_white_missing() {
  printf("\n=== TEST: graveyard white NOT tracked in pieces_ (user's bug) ===\n");
  uint8_t board[MAIN_COLS][MAIN_ROWS];
  buildEndOfGameBoard(board, /*trackGraveyardWhite=*/false);
  printBoard(board);

  Goal goals[PP_MAX_GOALS];
  int ng = runAssignment(board, goals);
  printf("  Assigned %d goals:\n", ng);
  for (int i = 0; i < ng; i++) {
    printf("    %s (%d,%d) -> (%d,%d)\n",
           goals[i].color, goals[i].fx, goals[i].fy, goals[i].tx, goals[i].ty);
  }

  CHECK(ng == 5, "untracked graveyard white -> only 5 goals (matches user's log)");

  PlanResult plan;
  bool ok = runPlan(board, goals, ng, plan);
  printf("  -> plan status=%d, num_steps=%d\n", (int)plan.status, plan.num_steps);
  CHECK(!ok, "planner CANNOT solve with the missing-white configuration");
  printf("  ^^^ this is why the user sees '5 moves' but the reset fails.\n");
}

static void test_single_goal_per_plan() {
  printf("\n=== TEST: single-goal plans stay ≤4 moves/timestep ===\n");
  printf("  (matches new resetBoard behaviour — process goals one-by-one)\n");

  uint8_t board[MAIN_COLS][MAIN_ROWS];
  buildEndOfGameBoard(board, /*trackGraveyardWhite=*/true);

  Goal goals[PP_MAX_GOALS];
  int ng = runAssignment(board, goals);
  CHECK(ng == 6, "6 goals for full end-of-game reset");

  // Simulate sequential single-goal planning
  uint8_t wide[PP_MAX_COLS][PP_MAX_ROWS];
  memset(wide, PP_NONE, sizeof(wide));
  for (int x = 0; x < MAIN_COLS; x++)
    for (int y = 0; y < MAIN_ROWS; y++) wide[x][y] = board[x][y];

  int maxMovesPerStep = 0;
  int solvedCount = 0;
  for (int g = 0; g < ng; g++) {
    // Skip goals already satisfied
    if (wide[goals[g].fx][goals[g].fy] == PP_NONE) continue;

    MoveGoal one = { goals[g].fx, goals[g].fy, goals[g].tx, goals[g].ty };
    PlanResult plan = planPath(wide, MAIN_COLS, MAIN_ROWS, &one, 1);
    if (plan.status != PlanResult::OK) {
      printf("  goal %d (%d,%d)->(%d,%d) NO PATH\n", g,
             goals[g].fx, goals[g].fy, goals[g].tx, goals[g].ty);
      continue;
    }
    solvedCount++;
    for (int t = 0; t < plan.num_steps; t++) {
      if (plan.steps[t].num_moves > maxMovesPerStep)
        maxMovesPerStep = plan.steps[t].num_moves;
    }
    printf("  goal %d %s (%d,%d)->(%d,%d): solved in %d timesteps, max %d moves/step\n",
           g, goals[g].color, goals[g].fx, goals[g].fy, goals[g].tx, goals[g].ty,
           plan.num_steps, plan.num_steps > 0 ? plan.steps[0].num_moves : 0);
  }

  CHECK(solvedCount == 6, "all 6 goals solvable one-at-a-time");
  CHECK(maxMovesPerStep <= 4, "every single-goal plan has ≤4 moves/step (fits MAX_MULTI_MOVES)");
}

static void test_only_graveyard_pieces() {
  printf("\n=== TEST: only graveyard pieces need to move ===\n");
  uint8_t board[MAIN_COLS][MAIN_ROWS];
  memset(board, PP_NONE, sizeof(board));
  board[0][0] = PP_WHITE;
  board[1][0] = PP_WHITE;
  board[0][2] = PP_BLACK;
  board[1][2] = PP_BLACK;
  board[3][0] = PP_BLACK;   // graveyard black
  board[3][1] = PP_WHITE;   // graveyard white
  printBoard(board);

  Goal goals[PP_MAX_GOALS];
  int ng = runAssignment(board, goals);
  printf("  Assigned %d goals:\n", ng);
  for (int i = 0; i < ng; i++) {
    printf("    %s (%d,%d) -> (%d,%d)\n",
           goals[i].color, goals[i].fx, goals[i].fy, goals[i].tx, goals[i].ty);
  }

  CHECK(ng == 2, "both graveyard pieces get goals");
  PlanResult plan;
  bool ok = runPlan(board, goals, ng, plan);
  CHECK(ok, "planner solves graveyard-only reset");
  printf("  -> plan status=%d, num_steps=%d\n", (int)plan.status, plan.num_steps);
}

int main() {
  printf("test_reset_board — verifies the resetBoard greedy + path planner\n");

  test_all_tracked();
  test_graveyard_white_missing();
  test_single_goal_per_plan();
  test_only_graveyard_pieces();

  printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
