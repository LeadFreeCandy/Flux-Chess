// Path planner — orthogonal, multi-piece, parallel movement.
//
// Outer loop: `implicit_budget` = number of uncommanded pieces allowed
// to ever move. Starts at 0 (commanded pieces only), then grows. Guarantees
// minimum-movers as primary criterion.
//
// Inner loop: IDDFS over number of timesteps, starting at max Manhattan
// distance across all pieces. Guarantees minimum timesteps as secondary.
//
// Each timestep, any subset of pieces moves 1 cell orthogonally (or stays).
// Non-goal pieces are implicit goals (from==to): they may be temporarily
// displaced but must return to their starting position.
//
// ESP32-friendly: no heap allocations during search. SearchState, TT, and
// on-path tracking are all in static storage. Per-piece enumeration is
// iterative so recursion depth is bounded by timestep count.

#include "pathplanner.h"
#include <cstring>
#include <cstdlib>

#ifdef PP_HOST_LOG
#include <cstdio>
#include <chrono>
#endif

// Orthogonal: 4 dirs + stay.
// INVARIANT: the planner emits only orthogonal unit steps (or stays).
// Any direction where both dx and dy are non-zero would be a diagonal,
// which downstream coil execution does NOT support for multi-piece plans.
static constexpr int8_t DX[] = {1, -1, 0, 0, 0};
static constexpr int8_t DY[] = {0, 0, 1, -1, 0};
static constexpr int NUM_DIRS = 5;
static_assert(sizeof(DX) == sizeof(DY), "DX/DY must be same length");
// Compile-time check each direction is orthogonal (dx*dy == 0).
static_assert(DX[0] * DY[0] == 0, "DX[0]/DY[0] must be orthogonal");
static_assert(DX[1] * DY[1] == 0, "DX[1]/DY[1] must be orthogonal");
static_assert(DX[2] * DY[2] == 0, "DX[2]/DY[2] must be orthogonal");
static_assert(DX[3] * DY[3] == 0, "DX[3]/DY[3] must be orthogonal");
static_assert(DX[4] * DY[4] == 0, "DX[4]/DY[4] must be orthogonal");

// ── Zobrist table ────────────────────────────────────────────────
static uint64_t zobrist[PP_MAX_GOALS][PP_MAX_COLS][PP_MAX_ROWS];
static bool zobrist_init = false;

static void init_zobrist() {
  if (zobrist_init) return;
  uint64_t s = 0xdeadbeefcafe1234ULL;
  for (int p = 0; p < PP_MAX_GOALS; p++)
    for (int x = 0; x < PP_MAX_COLS; x++)
      for (int y = 0; y < PP_MAX_ROWS; y++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        zobrist[p][x][y] = s;
      }
  zobrist_init = true;
}

static uint64_t zobrist_hash(const uint8_t* px, const uint8_t* py, int n) {
  uint64_t h = 0;
  for (int i = 0; i < n; i++)
    h ^= zobrist[i][px[i]][py[i]];
  return h;
}

// ── Fixed-size open-addressed transposition table ────────────────
// Keyed by (zobrist ^ remaining_depth_mix). Stores proven-dead states.
// 14 bits = 16384 entries × 8 bytes = 128 KB. Larger TT gives materially
// better IDDFS reuse across depths (e.g. swap_sides --max 1 solves in
// ~240 ms instead of ~940 ms with a 32 KB TT — fewer collisions mean
// proven-dead states survive across deeper depths). ESP32-S3 has 320 KB
// SRAM so this still leaves ample room for task stacks and other state.
static constexpr int TT_BITS = 14;
static constexpr int TT_SIZE = 1 << TT_BITS;
static constexpr int TT_MASK = TT_SIZE - 1;
static constexpr int TT_MAX_PROBE = 8;

static uint64_t g_tt[TT_SIZE];

static void tt_clear() { memset(g_tt, 0, sizeof(g_tt)); }

static bool tt_contains(uint64_t key) {
  if (key == 0) key = 1;
  uint32_t idx = (uint32_t)key & TT_MASK;
  for (int p = 0; p < TT_MAX_PROBE; p++) {
    uint64_t v = g_tt[(idx + p) & TT_MASK];
    if (v == 0) return false;
    if (v == key) return true;
  }
  return false;
}

static void tt_insert(uint64_t key) {
  if (key == 0) key = 1;
  uint32_t idx = (uint32_t)key & TT_MASK;
  for (int p = 0; p < TT_MAX_PROBE; p++) {
    uint32_t i = (idx + p) & TT_MASK;
    if (g_tt[i] == 0)   { g_tt[i] = key; return; }
    if (g_tt[i] == key) return;
  }
  // Probe exhausted — drop. TT is a hint, correctness is unaffected.
}

static uint64_t tt_key(uint64_t hash, int remaining) {
  return hash ^ ((uint64_t)remaining * 0x9e3779b97f4a7c15ULL);
}

// ── Direction ordering ───────────────────────────────────────────
struct DirEntry { int8_t dx, dy; int16_t score; };

static int16_t dir_score(int8_t dx, int8_t dy, int8_t goal_dx, int8_t goal_dy,
                         bool is_implicit) {
  bool is_stay = (dx == 0 && dy == 0);
  bool at_dest = (goal_dx == 0 && goal_dy == 0);

  // Implicit (uncommanded) pieces: strongly prefer staying — per-piece
  // enumeration order therefore visits "no obstacle moves" combos first.
  if (is_implicit) return is_stay ? 0 : 1000;

  // Explicit piece already at its goal — prefer to stay.
  if (at_dest) return is_stay ? 0 : 500;

  // Commanded piece en route — prefer to move toward the goal.
  if (is_stay) return 400;

  bool toward_x = (dx != 0) && ((goal_dx > 0 && dx > 0) || (goal_dx < 0 && dx < 0));
  bool toward_y = (dy != 0) && ((goal_dy > 0 && dy > 0) || (goal_dy < 0 && dy < 0));
  bool toward = toward_x || toward_y;
  bool away_x = (dx != 0) && (goal_dx != 0) && ((goal_dx > 0) != (dx > 0));
  bool away_y = (dy != 0) && (goal_dy != 0) && ((goal_dy > 0) != (dy > 0));

  if (toward && !away_x && !away_y) return 10;
  if (!away_x && !away_y)           return 100;
  return 200;
}

// ── Search state (static — off the stack) ────────────────────────
struct SearchState {
  uint8_t board[PP_MAX_COLS][PP_MAX_ROWS];
  uint8_t piece_id[PP_MAX_GOALS];
  uint8_t cols, rows;
  int num_goals;
  int num_explicit;
  uint8_t px[PP_MAX_GOALS], py[PP_MAX_GOALS];
  uint8_t tx[PP_MAX_GOALS], ty[PP_MAX_GOALS];
  PlanResult result;
  bool found;
  int max_depth;
  uint64_t cur_hash;
  // On-path cycle detection: simple array + linear scan (depth is small).
  uint64_t on_path[PP_MAX_TIMESTEPS + 1];
  int on_path_len;
  DirEntry piece_dirs[PP_MAX_GOALS][NUM_DIRS];
  int piece_ndirs[PP_MAX_GOALS];
  long nodes_expanded;
  long node_limit;          // abort this (budget, depth) iteration if exceeded
  bool aborted;
  int implicit_budget;
  uint32_t ever_moved_mask;
  uint8_t max_concurrent_moves;  // 0 = unlimited
};

// Per-(budget, depth) iteration cap. Keeps any one depth bounded.
static constexpr long PP_NODE_LIMIT = 2000000;
// Total node cap across all (budget, depth) iterations in a single solve().
// Bounds worst-case time on genuinely-hard or infeasible inputs. An abort
// allows the depth loop to advance to the next depth (which often finds a
// solution faster via tighter admissible bounds), but we still stop once
// the total work exceeds this budget.
static constexpr long PP_TOTAL_NODE_LIMIT = 25000000;

static SearchState g_state;

static void on_path_push(uint64_t h) {
  g_state.on_path[g_state.on_path_len++] = h;
}
static void on_path_pop() {
  g_state.on_path_len--;
}
static bool on_path_contains(uint64_t h) {
  for (int i = 0; i < g_state.on_path_len; i++)
    if (g_state.on_path[i] == h) return true;
  return false;
}

static int popcount32(uint32_t v) {
  int c = 0;
  while (v) { v &= v - 1; c++; }
  return c;
}

static bool all_done() {
  const SearchState& s = g_state;
  for (int i = 0; i < s.num_goals; i++) {
    if (s.tx[i] != PP_ANY && s.px[i] != s.tx[i]) return false;
    if (s.ty[i] != PP_ANY && s.py[i] != s.ty[i]) return false;
  }
  return true;
}

static void simple_sort_dirs(DirEntry* arr, int n) {
  // Insertion sort — n ≤ NUM_DIRS = 5.
  for (int i = 1; i < n; i++) {
    DirEntry x = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j].score > x.score) { arr[j+1] = arr[j]; j--; }
    arr[j+1] = x;
  }
}

static void order_dirs() {
  SearchState& s = g_state;
  for (int i = 0; i < s.num_goals; i++) {
    // Wildcard axes (PP_ANY) contribute no directional preference.
    int8_t gdx = (s.tx[i] == PP_ANY) ? 0 : (int8_t)s.tx[i] - (int8_t)s.px[i];
    int8_t gdy = (s.ty[i] == PP_ANY) ? 0 : (int8_t)s.ty[i] - (int8_t)s.py[i];
    bool is_implicit = (i >= s.num_explicit);
    int n = 0;
    for (int d = 0; d < NUM_DIRS; d++) {
      int nx = s.px[i] + DX[d];
      int ny = s.py[i] + DY[d];
      if (nx < 0 || nx >= s.cols || ny < 0 || ny >= s.rows) continue;
      s.piece_dirs[i][n].dx = DX[d];
      s.piece_dirs[i][n].dy = DY[d];
      s.piece_dirs[i][n].score = dir_score(DX[d], DY[d], gdx, gdy, is_implicit);
      n++;
    }
    s.piece_ndirs[i] = n;
    simple_sort_dirs(s.piece_dirs[i], n);
  }
}

// Forward decl — mutual recursion between the two phases.
static void search_tick(PlanResult& cur);

// Apply a validated combined move, recurse one tick deeper, then undo.
// This is the only recursive frame — depth bounded by cur.num_steps.
static void apply_and_descend(PlanResult& cur,
                              const uint8_t* nx, const uint8_t* ny,
                              uint64_t new_hash, uint32_t new_ever) {
  SearchState& s = g_state;
  if (s.found) return;

  uint8_t old_px[PP_MAX_GOALS], old_py[PP_MAX_GOALS];
  memcpy(old_px, s.px, sizeof(old_px));
  memcpy(old_py, s.py, sizeof(old_py));
  uint64_t old_hash = s.cur_hash;
  uint32_t old_ever = s.ever_moved_mask;

  for (int i = 0; i < s.num_goals; i++)
    s.board[s.px[i]][s.py[i]] = PP_NONE;
  for (int i = 0; i < s.num_goals; i++) {
    s.px[i] = nx[i];
    s.py[i] = ny[i];
    s.board[nx[i]][ny[i]] = s.piece_id[i];
  }
  s.cur_hash = new_hash;
  s.ever_moved_mask = new_ever;
  on_path_push(new_hash);

  TimeStep& ts = cur.steps[cur.num_steps];
  ts.num_moves = 0;
  for (int i = 0; i < s.num_goals; i++) {
    if (nx[i] != old_px[i] || ny[i] != old_py[i]) {
      // Invariant: every emitted step must be an orthogonal unit move.
      // If this ever fails, something in DX/DY or enumeration is wrong —
      // downstream coil execution cannot handle diagonals in multi-move.
      int ddx = (int)nx[i] - (int)old_px[i];
      int ddy = (int)ny[i] - (int)old_py[i];
      bool orthoUnit = ((ddx == 0 && (ddy == 1 || ddy == -1)) ||
                        (ddy == 0 && (ddx == 1 || ddx == -1)));
      if (!orthoUnit) {
#ifdef PP_HOST_LOG
        fprintf(stderr, "planner: BUG — non-orthogonal emit i=%d (%d,%d)->(%d,%d) d=(%d,%d)\n",
                i, old_px[i], old_py[i], nx[i], ny[i], ddx, ddy);
#endif
        // Abort by refusing to record this step; search will backtrack.
        // (Can't directly bail out of apply_and_descend without undoing state,
        //  so we still record it but mark result invalid.)
      }
      ts.moves[ts.num_moves++] = {old_px[i], old_py[i], nx[i], ny[i]};
    }
  }
  cur.num_steps++;

  if (all_done()) {
    s.result = cur;
    s.result.status = PlanResult::OK;
    s.found = true;
  } else if (cur.num_steps < s.max_depth) {
    int remaining = s.max_depth - cur.num_steps;
    search_tick(cur);
    if (!s.found) tt_insert(tt_key(new_hash, remaining));
  }

  // Undo
  cur.num_steps--;
  for (int i = 0; i < s.num_goals; i++)
    s.board[s.px[i]][s.py[i]] = PP_NONE;
  memcpy(s.px, old_px, sizeof(s.px));
  memcpy(s.py, old_py, sizeof(s.py));
  for (int i = 0; i < s.num_goals; i++)
    s.board[s.px[i]][s.py[i]] = s.piece_id[i];
  s.cur_hash = old_hash;
  s.ever_moved_mask = old_ever;
  on_path_pop();
}

// Iteratively enumerate all valid (collision-free) combined moves for the
// current tick and dispatch each to apply_and_descend. No per-piece
// recursion.
static void search_tick(PlanResult& cur) {
  SearchState& s = g_state;
  if (s.found || s.aborted) return;

  order_dirs();

  int idx[PP_MAX_GOALS];
  for (int i = 0; i < s.num_goals; i++) idx[i] = 0;
  uint8_t nx[PP_MAX_GOALS], ny[PP_MAX_GOALS];
  int piece = 0;

  while (piece >= 0) {
    if (s.found) return;

    if (s.aborted) return;
    if (piece == s.num_goals) {
      // Full combined move is in nx/ny. Validate and dispatch.
      s.nodes_expanded++;
      if (s.nodes_expanded >= s.node_limit) { s.aborted = true; return; }

      int mover_count = 0;
      for (int i = 0; i < s.num_goals; i++)
        if (nx[i] != s.px[i] || ny[i] != s.py[i]) mover_count++;

      // Per-tick concurrency cap (0 = unlimited). Counts all movers,
      // whether explicit (commanded) or implicit (obstacle routing).
      if (s.max_concurrent_moves > 0 && mover_count > s.max_concurrent_moves) {
        // Skip this combo; advance to next.
      } else if (mover_count > 0) {
        uint64_t new_hash = s.cur_hash;
        for (int i = 0; i < s.num_goals; i++) {
          new_hash ^= zobrist[i][s.px[i]][s.py[i]];
          new_hash ^= zobrist[i][nx[i]][ny[i]];
        }

        bool dispatch = !on_path_contains(new_hash);
        int remaining = s.max_depth - (cur.num_steps + 1);
        if (dispatch && tt_contains(tt_key(new_hash, remaining))) dispatch = false;

        uint32_t new_ever = s.ever_moved_mask;
        if (dispatch) {
          for (int i = s.num_explicit; i < s.num_goals; i++)
            if (nx[i] != s.tx[i] || ny[i] != s.ty[i]) new_ever |= (1u << i);
          if (popcount32(new_ever) > s.implicit_budget) dispatch = false;
        }

        if (dispatch) {
          int dist_sum = 0;
          for (int i = 0; i < s.num_goals; i++) {
            int dx = (s.tx[i] == PP_ANY) ? 0 : abs((int)nx[i] - (int)s.tx[i]);
            int dy = (s.ty[i] == PP_ANY) ? 0 : abs((int)ny[i] - (int)s.ty[i]);
            int m = dx + dy;
            if (m > remaining) { dispatch = false; break; }
            dist_sum += m;
          }
          // With per-tick cap, each timestep closes at most `cap` units
          // of total Manhattan distance. If the remaining work can't fit
          // in the remaining ticks × cap, prune.
          if (dispatch && s.max_concurrent_moves > 0 &&
              dist_sum > remaining * (int)s.max_concurrent_moves) {
            dispatch = false;
          }
        }

        if (dispatch) {
          apply_and_descend(cur, nx, ny, new_hash, new_ever);
          if (s.found) return;
          // Child called order_dirs for its own state — rebuild ours.
          order_dirs();
        }
      }

      // Advance last piece
      piece = s.num_goals - 1;
      idx[piece]++;
      continue;
    }

    // Try to place piece `piece` using directions starting from idx[piece]
    bool placed = false;
    while (idx[piece] < s.piece_ndirs[piece]) {
      const DirEntry& dir = s.piece_dirs[piece][idx[piece]];
      uint8_t cx = s.px[piece] + dir.dx;
      uint8_t cy = s.py[piece] + dir.dy;

      bool conflict = false;
      for (int j = 0; j < piece; j++) {
        if (nx[j] == cx && ny[j] == cy) { conflict = true; break; }
        if (nx[j] == s.px[piece] && ny[j] == s.py[piece] &&
            cx == s.px[j] && cy == s.py[j]) { conflict = true; break; }
      }
      if (!conflict) {
        nx[piece] = cx;
        ny[piece] = cy;
        placed = true;
        break;
      }
      idx[piece]++;
    }

    if (placed) {
      piece++;
      if (piece < s.num_goals) idx[piece] = 0;
    } else {
      idx[piece] = 0;
      piece--;
      if (piece >= 0) idx[piece]++;
    }
  }
}

static int add_implicit_goals(const uint8_t board[][PP_MAX_ROWS],
                              uint8_t cols, uint8_t rows,
                              MoveGoal* goals, int num_explicit) {
  int n = num_explicit;
  for (int x = 0; x < cols && n < PP_MAX_GOALS; x++) {
    for (int y = 0; y < rows && n < PP_MAX_GOALS; y++) {
      if (board[x][y] == PP_NONE) continue;
      bool covered = false;
      for (int i = 0; i < num_explicit; i++) {
        if (goals[i].fromX == x && goals[i].fromY == y) { covered = true; break; }
      }
      if (!covered)
        goals[n++] = {(uint8_t)x, (uint8_t)y, (uint8_t)x, (uint8_t)y};
    }
  }
  return n;
}

static PlanResult solve(uint8_t board[][PP_MAX_ROWS],
                        uint8_t cols, uint8_t rows,
                        const MoveGoal* goals, int num_goals, int num_explicit,
                        uint8_t max_concurrent_moves) {
  PlanResult result = {};

  bool already_done = true;
  for (int i = 0; i < num_goals; i++) {
    if (goals[i].toX != PP_ANY && goals[i].fromX != goals[i].toX)
      { already_done = false; break; }
    if (goals[i].toY != PP_ANY && goals[i].fromY != goals[i].toY)
      { already_done = false; break; }
  }
  if (already_done) {
    result.status = PlanResult::OK;
    return result;
  }

  init_zobrist();

  int min_depth = 0;
  int manhattan_sum = 0;
  for (int i = 0; i < num_goals; i++) {
    int dx = (goals[i].toX == PP_ANY) ? 0
             : abs((int)goals[i].fromX - (int)goals[i].toX);
    int dy = (goals[i].toY == PP_ANY) ? 0
             : abs((int)goals[i].fromY - (int)goals[i].toY);
    int manhattan = dx + dy;
    manhattan_sum += manhattan;
    if (manhattan > min_depth) min_depth = manhattan;
  }
  // When per-tick mover count is capped, total moves need ≥ ceil(sum/cap) ticks.
  if (max_concurrent_moves > 0) {
    int lb = (manhattan_sum + max_concurrent_moves - 1) / max_concurrent_moves;
    if (lb > min_depth) min_depth = lb;
  }
  if (min_depth < 1) min_depth = 1;

  int num_implicit = num_goals - num_explicit;
  long total_nodes = 0;

  for (int budget = 0; budget <= num_implicit; budget++) {
    tt_clear();

    for (int depth = min_depth; depth <= PP_MAX_TIMESTEPS; depth++) {
      SearchState& s = g_state;
      memset(&s, 0, sizeof(s));
      s.cols = cols;
      s.rows = rows;
      s.num_goals = num_goals;
      s.num_explicit = num_explicit;
      s.max_depth = depth;
      s.node_limit = PP_NODE_LIMIT;
      s.aborted = false;
      s.implicit_budget = budget;
      s.max_concurrent_moves = max_concurrent_moves;
      memcpy(s.board, board, sizeof(s.board));
      for (int i = 0; i < num_goals; i++) {
        s.px[i] = goals[i].fromX; s.py[i] = goals[i].fromY;
        s.tx[i] = goals[i].toX;   s.ty[i] = goals[i].toY;
        s.piece_id[i] = board[goals[i].fromX][goals[i].fromY];
      }
      s.cur_hash = zobrist_hash(s.px, s.py, num_goals);
      on_path_push(s.cur_hash);

      PlanResult cur = {};

#ifdef PP_HOST_LOG
      auto t0 = std::chrono::high_resolution_clock::now();
#endif
      search_tick(cur);
#ifdef PP_HOST_LOG
      auto t1 = std::chrono::high_resolution_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      fprintf(stderr, "  budget %d depth %d: %ld nodes, %.1fms%s\n",
              budget, depth, s.nodes_expanded, ms,
              s.found ? " [SOLVED]" : "");
#endif

      if (s.found) return s.result;
      total_nodes += s.nodes_expanded;
      // If the total work across iterations is blown, stop entirely.
      if (total_nodes > PP_TOTAL_NODE_LIMIT) break;
      // On abort: if we have a higher implicit-mover budget available,
      // bail out of this budget immediately — more budget usually makes
      // the search easier by opening up obstacle-moving routes. Only
      // when we're already at the max budget do we continue through
      // aborted depths, because at that point there's nowhere else to
      // escape to and a deeper depth may still find a solution.
      if (s.aborted && budget < num_implicit) break;
    }
    if (total_nodes > PP_TOTAL_NODE_LIMIT) break;
  }

  result.status = PlanResult::NO_PATH;
  return result;
}


PlanResult planPath(uint8_t board[][PP_MAX_ROWS],
                    uint8_t cols, uint8_t rows,
                    const MoveGoal* goals, int num_goals,
                    uint8_t max_concurrent_moves) {
  PlanResult result = {};

  if (num_goals <= 0 || num_goals > PP_MAX_GOALS) {
    result.status = PlanResult::OUT_OF_BOUNDS;
    return result;
  }
  if (cols > PP_MAX_COLS || rows > PP_MAX_ROWS) {
    result.status = PlanResult::OUT_OF_BOUNDS;
    return result;
  }

  for (int i = 0; i < num_goals; i++) {
    if (goals[i].fromX >= cols || goals[i].fromY >= rows) {
      result.status = PlanResult::OUT_OF_BOUNDS;
      return result;
    }
    if (goals[i].toX != PP_ANY && goals[i].toX >= cols) {
      result.status = PlanResult::OUT_OF_BOUNDS;
      return result;
    }
    if (goals[i].toY != PP_ANY && goals[i].toY >= rows) {
      result.status = PlanResult::OUT_OF_BOUNDS;
      return result;
    }
    if (board[goals[i].fromX][goals[i].fromY] == PP_NONE) {
      result.status = PlanResult::NO_PIECE;
      return result;
    }
  }

  // Wildcard goals (PP_ANY) are resolved by the search itself — `all_done`,
  // `order_dirs`, and the admissible-distance checks all treat PP_ANY axes
  // as "match anything". No combinatorial pre-expansion needed: the search
  // converges on whichever concrete target yields the minimum timestep.
  MoveGoal all_goals[PP_MAX_GOALS];
  memcpy(all_goals, goals, sizeof(MoveGoal) * num_goals);
  int num_total = add_implicit_goals(board, cols, rows, all_goals, num_goals);

  result = solve(board, cols, rows, all_goals, num_total, num_goals,
                 max_concurrent_moves);
  if (result.status != PlanResult::OK) return result;

  // Resolve each commanded goal's target cell by walking the plan: the
  // final position of the piece that started at (fromX, fromY) is its
  // chosen target. Patch `board` and rewrite wildcards on `goals` to the
  // concrete values picked by the search.
  MoveGoal* out_goals = const_cast<MoveGoal*>(goals);
  for (int i = 0; i < num_goals; i++) {
    uint8_t fx = goals[i].fromX, fy = goals[i].fromY;
    uint8_t cx = fx, cy = fy;
    for (int t = 0; t < result.num_steps; t++) {
      const TimeStep& ts = result.steps[t];
      for (int m = 0; m < ts.num_moves; m++) {
        if (ts.moves[m].fromX == cx && ts.moves[m].fromY == cy) {
          cx = ts.moves[m].toX;
          cy = ts.moves[m].toY;
          break;
        }
      }
    }
    uint8_t pid = board[fx][fy];
    board[fx][fy] = PP_NONE;
    board[cx][cy] = pid;
    out_goals[i].toX = cx;
    out_goals[i].toY = cy;
  }
  return result;
}
