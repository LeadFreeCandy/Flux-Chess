#pragma once
// Runtime accessor for the precomputed hexapawn plan table.
//
// The table is generated offline by tests/gen_hexapawn_tables.cpp and
// committed as hexapawn_tables.h. Each entry is keyed by a packed
// (play_board, graveyard_contents) state and holds either:
//   - is_reset = 1 → one plan that returns every piece to its home, or
//   - is_reset = 0 → one or more plans, each being an optimal AI move.
//
// Plans are serialized as:
//   uint8  num_steps
//   for each step:
//     uint8 num_moves
//     num_moves * { fromX, fromY, toX, toY }   (main-grid coords)

#include "hexapawn.h"
#include "pathplanner.h"
#include "hexapawn_tables.h"

namespace hex_table {

// Packs (play[c][r] ∈ {0,1,2}) × 9 cells + graveyard col-3 (3 cells) into
// a radix-3 key matching gen_hexapawn_tables.cpp::pack_state().
//
// play_row_from_grid[c] at hexapawn row r comes from the pieces_ grid at
// subdivided coord (c*3, r*3). Pass in those 9 values. grave[r] comes from
// the pieces_ grid at (9, r*3).
static inline uint32_t packKey(const uint8_t play[HP_SIZE][HP_SIZE],
                               const uint8_t grave[HP_SIZE]) {
  uint32_t k = 0, m = 1;
  for (int c = 0; c < HP_SIZE; c++)
    for (int r = 0; r < HP_SIZE; r++) {
      k += (play[c][r] & 0x03) * m;
      m *= 3;
    }
  for (int r = 0; r < HP_SIZE; r++) {
    k += (grave[r] & 0x03) * m;
    m *= 3;
  }
  return k;
}

// Binary-search for a key. Returns nullptr if not found.
static inline const HexTableEntry* lookup(uint32_t key) {
  uint16_t lo = 0, hi = g_hex_num_entries;
  while (lo < hi) {
    uint16_t mid = lo + (hi - lo) / 2;
    uint32_t mk = g_hex_entries[mid].key;
    if (mk == key) return &g_hex_entries[mid];
    if (mk < key) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

// Parse the serialized plan at g_hex_plan_bytes[offset] into `out`.
// Returns true on success.
static inline bool decodePlan(uint32_t offset, PlanResult& out) {
  const uint8_t* p = &g_hex_plan_bytes[offset];
  uint8_t num_steps = *p++;
  if (num_steps > PP_MAX_TIMESTEPS) return false;
  out.status = PlanResult::OK;
  out.num_steps = num_steps;
  for (int t = 0; t < num_steps; t++) {
    uint8_t num_moves = *p++;
    if (num_moves > PP_MAX_GOALS) return false;
    out.steps[t].num_moves = num_moves;
    for (int i = 0; i < num_moves; i++) {
      out.steps[t].moves[i].fromX = *p++;
      out.steps[t].moves[i].fromY = *p++;
      out.steps[t].moves[i].toX   = *p++;
      out.steps[t].moves[i].toY   = *p++;
    }
  }
  return true;
}

// Fetch a plan for the given key. `plan_pick` selects which plan when the
// entry has multiple (AI move ties). Returns status OK and fills `plan`
// on success; sets `is_reset` so the caller knows which path to take.
// Returns false if no entry or decode fails.
static inline bool fetchPlan(uint32_t key, uint8_t plan_pick,
                             PlanResult& plan, bool& is_reset) {
  const HexTableEntry* e = lookup(key);
  if (!e) return false;
  is_reset = (e->is_reset != 0);
  uint8_t n = e->num_plans;
  if (n == 0) return false;
  uint8_t idx = (uint8_t)(plan_pick % n);
  uint32_t off = g_hex_plan_offsets[e->first_plan_idx + idx];
  return decodePlan(off, plan);
}

}  // namespace hex_table
