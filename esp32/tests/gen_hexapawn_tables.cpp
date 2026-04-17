// Offline generator for hexapawn_tables.h.
//
// Walks every reachable game position from the initial configuration,
// branching on:
//   - every legal human move,
//   - every legal graveyard-slot choice the human could make on a capture,
//   - every optimal AI move (ties produce multiple entries).
//
// For each position the generator records:
//   - if game is over: a single plan that resets the board to its starting
//     state (flag = RESET), or
//   - if it's the AI's turn: one plan per optimal AI move (flag = AI).
//
// Output: esp32/firmware/hexapawn_tables.h (committed to the tree).
//
// Build: g++ -std=c++17 -O2 -I../firmware -o gen_hexapawn_tables
//          gen_hexapawn_tables.cpp ../firmware/pathplanner.cpp
// Run:   ./gen_hexapawn_tables > ../firmware/hexapawn_tables.h

#include "hexapawn.h"
#include "pathplanner.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <queue>
#include <unordered_map>
#include <algorithm>
#include <cassert>

// ── State key ──────────────────────────────────────────────
// play board 3x3 + graveyard col 3 (3 cells). Each cell radix-3:
// 0 = empty, 1 = white, 2 = black. 12 cells total → value ≤ 3^12 = 531441.
// Turn is NOT part of the key: we only ever look up states the firmware
// queries, which are always "after the human moves, before the AI acts"
// (AI-turn) or "game over".

struct GameState {
  uint8_t play[HP_SIZE][HP_SIZE];  // play[c][r]
  uint8_t grave[HP_SIZE];          // grave[row], row in {0,1,2} at grid col 3
  uint8_t turn;                    // HP_WHITE or HP_BLACK
};

static uint32_t pack_state(const GameState& s) {
  uint32_t k = 0, m = 1;
  for (int c = 0; c < HP_SIZE; c++)
    for (int r = 0; r < HP_SIZE; r++) {
      k += s.play[c][r] * m;
      m *= 3;
    }
  for (int r = 0; r < HP_SIZE; r++) {
    k += s.grave[r] * m;
    m *= 3;
  }
  return k;
}

static bool game_over(const GameState& s) {
  Hexapawn g;
  memcpy(g.board, s.play, sizeof(g.board));
  g.turn = s.turn;
  g.winner = HP_NONE;
  g.checkWin();
  return g.winner != HP_NONE;
}

// ── Plan encoding ──────────────────────────────────────────
// Each plan serialises to:
//   uint8 num_steps,
//   for each step: uint8 num_moves, then num_moves * { fromX, fromY, toX, toY }
// All coords are in MAIN-grid space (0..MAIN_COLS-1, 0..MAIN_ROWS-1).

static constexpr uint8_t MAIN_COLS = 4;
static constexpr uint8_t MAIN_ROWS = 3;
static constexpr uint8_t MAIN_GRAVE_COL = 3;

static std::vector<uint8_t> serialize_plan(const PlanResult& p) {
  std::vector<uint8_t> out;
  out.push_back((uint8_t)p.num_steps);
  for (int t = 0; t < p.num_steps; t++) {
    const TimeStep& ts = p.steps[t];
    out.push_back((uint8_t)ts.num_moves);
    for (int i = 0; i < ts.num_moves; i++) {
      const PieceMove& m = ts.moves[i];
      out.push_back(m.fromX);
      out.push_back(m.fromY);
      out.push_back(m.toX);
      out.push_back(m.toY);
    }
  }
  return out;
}

// ── Converting GameState → planner inputs ──────────────────

static void state_to_board(const GameState& s, uint8_t board[][PP_MAX_ROWS]) {
  memset(board, PP_NONE, PP_MAX_COLS * PP_MAX_ROWS);
  for (int c = 0; c < HP_SIZE; c++)
    for (int r = 0; r < HP_SIZE; r++) {
      if (s.play[c][r] == HP_WHITE) board[c][r] = PP_WHITE;
      else if (s.play[c][r] == HP_BLACK) board[c][r] = PP_BLACK;
    }
  for (int r = 0; r < HP_SIZE; r++) {
    if (s.grave[r] == HP_WHITE) board[MAIN_GRAVE_COL][r] = PP_WHITE;
    else if (s.grave[r] == HP_BLACK) board[MAIN_GRAVE_COL][r] = PP_BLACK;
  }
}

static bool plan_ai_move(const GameState& s, const HexapawnMove& m,
                         PlanResult& out_plan,
                         GameState& out_next) {
  uint8_t board[PP_MAX_COLS][PP_MAX_ROWS];
  state_to_board(s, board);

  uint8_t src_piece = s.play[m.fc][m.fr];
  uint8_t dst_piece = s.play[m.tc][m.tr];
  bool capture = (dst_piece != HP_NONE && dst_piece != src_piece);

  MoveGoal goals[PP_MAX_GOALS];
  int n = 0;
  goals[n++] = { (uint8_t)m.fc, (uint8_t)m.fr,
                 (uint8_t)m.tc, (uint8_t)m.tr };
  if (capture) {
    goals[n++] = { (uint8_t)m.tc, (uint8_t)m.tr,
                   MAIN_GRAVE_COL, PP_ANY };
  }

  PlanResult p = planPath(board, MAIN_COLS, MAIN_ROWS, goals, n, 0);
  if (p.status != PlanResult::OK) return false;

  // Reconstruct the post-plan game state.
  out_next = s;
  out_next.play[m.fc][m.fr] = HP_NONE;
  out_next.play[m.tc][m.tr] = src_piece;
  if (capture) {
    uint8_t grave_row = goals[1].toY;  // planner picks concrete value
    out_next.grave[grave_row] = dst_piece;
  }
  out_next.turn = (s.turn == HP_WHITE) ? HP_BLACK : HP_WHITE;

  out_plan = p;
  return true;
}

// ── Reset plan ─────────────────────────────────────────────
// Build a plan that returns every piece to its starting square. Within a
// colour pieces are fungible so we use PP_ANY on the column axis: "any
// row-0 cell for whites, any row-2 cell for blacks".

static bool plan_reset(const GameState& s, PlanResult& out_plan) {
  uint8_t board[PP_MAX_COLS][PP_MAX_ROWS];
  state_to_board(s, board);

  MoveGoal goals[PP_MAX_GOALS];
  int n = 0;
  for (int c = 0; c < HP_SIZE; c++)
    for (int r = 0; r < HP_SIZE; r++) {
      if (s.play[c][r] == HP_WHITE)
        goals[n++] = { (uint8_t)c, (uint8_t)r, PP_ANY, 0 };
      else if (s.play[c][r] == HP_BLACK)
        goals[n++] = { (uint8_t)c, (uint8_t)r, PP_ANY, 2 };
    }
  for (int r = 0; r < HP_SIZE; r++) {
    if (s.grave[r] == HP_WHITE)
      goals[n++] = { MAIN_GRAVE_COL, (uint8_t)r, PP_ANY, 0 };
    else if (s.grave[r] == HP_BLACK)
      goals[n++] = { MAIN_GRAVE_COL, (uint8_t)r, PP_ANY, 2 };
  }

  PlanResult p = planPath(board, MAIN_COLS, MAIN_ROWS, goals, n, 0);
  if (p.status != PlanResult::OK) return false;
  out_plan = p;
  return true;
}

// ── Table entry ────────────────────────────────────────────
struct Entry {
  uint32_t key;
  bool is_reset;
  std::vector<std::vector<uint8_t>> plans;  // each inner = serialized plan
};

// ── BFS ────────────────────────────────────────────────────

int main() {
  fprintf(stderr, "gen_hexapawn_tables: starting BFS...\n");

  std::unordered_map<uint32_t, Entry> table;
  std::queue<GameState> q;

  GameState initial = {};
  for (int c = 0; c < HP_SIZE; c++) {
    initial.play[c][0] = HP_WHITE;
    initial.play[c][2] = HP_BLACK;
  }
  initial.turn = HP_WHITE;
  q.push(initial);

  int ai_states = 0, reset_states = 0, human_states = 0;

  while (!q.empty()) {
    GameState s = q.front(); q.pop();
    uint32_t k = pack_state(s);

    if (table.count(k)) continue;  // already processed this state

    if (game_over(s)) {
      PlanResult plan;
      if (!plan_reset(s, plan)) {
        fprintf(stderr, "  WARN: reset plan failed for key 0x%x\n", k);
        continue;
      }
      Entry e;
      e.key = k;
      e.is_reset = true;
      e.plans.push_back(serialize_plan(plan));
      table[k] = std::move(e);
      reset_states++;
      continue;
    }

    if (s.turn == HP_BLACK) {
      // AI to move.
      Hexapawn hg;
      memcpy(hg.board, s.play, sizeof(hg.board));
      hg.turn = s.turn;
      hg.winner = HP_NONE;

      HexapawnMove moves[18];
      int n = hg.computeAllOptimalMoves(moves, 18);
      if (n == 0) {
        // Shouldn't happen — checkWin should have caught stalemate.
        continue;
      }

      Entry e;
      e.key = k;
      e.is_reset = false;

      for (int i = 0; i < n; i++) {
        PlanResult plan;
        GameState next;
        if (!plan_ai_move(s, moves[i], plan, next)) {
          fprintf(stderr, "  WARN: AI plan failed for key 0x%x move %d\n", k, i);
          continue;
        }
        e.plans.push_back(serialize_plan(plan));
        q.push(next);
      }
      if (!e.plans.empty()) {
        table[k] = std::move(e);
        ai_states++;
      }
      continue;
    }

    // Human to move. We don't emit any table entry (the firmware doesn't
    // look up human-turn states). But we do enumerate successor states so
    // the BFS reaches all downstream AI and reset positions.
    human_states++;
    Hexapawn hg;
    memcpy(hg.board, s.play, sizeof(hg.board));
    hg.turn = s.turn;
    hg.winner = HP_NONE;

    HexapawnMove moves[18];
    int n = hg.getValidMoves(s.turn, moves);
    for (int i = 0; i < n; i++) {
      const HexapawnMove& m = moves[i];
      uint8_t src = s.play[m.fc][m.fr];
      uint8_t dst = s.play[m.tc][m.tr];
      bool capture = (dst != HP_NONE && dst != src);

      // Base successor (non-capture or capture with a specific slot choice).
      auto apply_base = [&](GameState& ns) {
        ns = s;
        ns.play[m.fc][m.fr] = HP_NONE;
        ns.play[m.tc][m.tr] = src;
        ns.turn = (s.turn == HP_WHITE) ? HP_BLACK : HP_WHITE;
      };

      if (!capture) {
        GameState ns;
        apply_base(ns);
        q.push(ns);
      } else {
        // Human picks which empty graveyard slot receives the captured piece.
        // Enumerate every empty slot and branch.
        for (int gr = 0; gr < HP_SIZE; gr++) {
          if (s.grave[gr] != HP_NONE) continue;
          GameState ns;
          apply_base(ns);
          ns.grave[gr] = dst;  // captured piece lives there now
          q.push(ns);
        }
      }
    }
  }

  fprintf(stderr, "  visited: %d AI states, %d reset states, %d human states (not emitted)\n",
          ai_states, reset_states, human_states);
  fprintf(stderr, "  total table entries: %zu\n", table.size());

  // ── Emit the header ──────────────────────────────────────
  // Sort by key for binary search at runtime. Concatenate plan bytes into
  // one flat array. Per-entry: key, first_plan_idx, num_plans, is_reset.
  // Per-plan: offset into plan_bytes.

  std::vector<const Entry*> sorted;
  sorted.reserve(table.size());
  for (auto& kv : table) sorted.push_back(&kv.second);
  std::sort(sorted.begin(), sorted.end(),
            [](const Entry* a, const Entry* b) { return a->key < b->key; });

  std::vector<uint32_t> plan_offsets;  // offset of each plan in plan_bytes
  std::vector<uint8_t>  plan_bytes;
  for (const Entry* e : sorted) {
    for (const auto& p : e->plans) {
      plan_offsets.push_back((uint32_t)plan_bytes.size());
      plan_bytes.insert(plan_bytes.end(), p.begin(), p.end());
    }
  }

  fprintf(stderr, "  plan_offsets: %zu (%zu bytes)\n",
          plan_offsets.size(), plan_offsets.size() * 2);
  fprintf(stderr, "  plan_bytes:   %zu\n", plan_bytes.size());

  printf("// GENERATED by tests/gen_hexapawn_tables.cpp. Do not edit.\n");
  printf("//\n");
  printf("// Entries: %zu (AI=%d, reset=%d)\n", sorted.size(), ai_states, reset_states);
  printf("// Plans:   %zu\n", plan_offsets.size());
  printf("// Plan bytes: %zu\n", plan_bytes.size());
  printf("\n");
  printf("#pragma once\n");
  printf("#include <cstdint>\n\n");

  printf("struct HexTableEntry {\n");
  printf("  uint32_t key;\n");
  printf("  uint16_t first_plan_idx;\n");
  printf("  uint8_t  num_plans;\n");
  printf("  uint8_t  is_reset;\n");
  printf("};\n\n");

  printf("static const uint16_t g_hex_num_entries = %zu;\n\n", sorted.size());

  printf("static const HexTableEntry g_hex_entries[] = {\n");
  uint32_t plan_idx = 0;
  for (const Entry* e : sorted) {
    printf("  { 0x%08x, %u, %u, %u },\n",
           e->key, plan_idx, (unsigned)e->plans.size(), (unsigned)(e->is_reset ? 1 : 0));
    plan_idx += e->plans.size();
  }
  printf("};\n\n");

  printf("static const uint32_t g_hex_plan_offsets[] = {\n");
  for (size_t i = 0; i < plan_offsets.size(); i++) {
    if (i % 8 == 0) printf("  ");
    printf("%u,", plan_offsets[i]);
    if (i % 8 == 7) printf("\n"); else printf(" ");
  }
  if (plan_offsets.size() % 8 != 0) printf("\n");
  printf("};\n\n");

  printf("static const uint8_t g_hex_plan_bytes[] = {\n");
  for (size_t i = 0; i < plan_bytes.size(); i++) {
    if (i % 16 == 0) printf("  ");
    printf("0x%02x,", plan_bytes[i]);
    if (i % 16 == 15) printf("\n"); else printf(" ");
  }
  if (plan_bytes.size() % 16 != 0) printf("\n");
  printf("};\n");

  fprintf(stderr, "done.\n");
  return 0;
}
