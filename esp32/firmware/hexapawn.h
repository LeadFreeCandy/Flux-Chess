#pragma once
#include <stdint.h>

// Hexapawn (3x3 pawn game). White moves up (+y), Black moves down (-y).
// White starts. This file is host-portable: no Arduino dependencies.

#define HP_NONE  0
#define HP_WHITE 1
#define HP_BLACK 2

static constexpr int HP_SIZE = 3;

struct HexapawnMove {
  int8_t fc, fr, tc, tr;  // game coords (0-2)
};

class Hexapawn {
public:
  int8_t board[HP_SIZE][HP_SIZE];  // [col][row], row 0=bottom (white start)
  uint8_t turn;
  uint8_t winner;

  void reset() {
    for (int c = 0; c < HP_SIZE; c++) {
      board[c][0] = HP_WHITE;
      board[c][1] = HP_NONE;
      board[c][2] = HP_BLACK;
    }
    turn = HP_WHITE;
    winner = HP_NONE;
  }

  int getValidMoves(uint8_t color, HexapawnMove* out, int maxMoves = 18) const {
    int n = 0;
    int dir = (color == HP_WHITE) ? 1 : -1;
    uint8_t enemy = (color == HP_WHITE) ? HP_BLACK : HP_WHITE;

    for (int c = 0; c < HP_SIZE && n < maxMoves; c++) {
      for (int r = 0; r < HP_SIZE && n < maxMoves; r++) {
        if (board[c][r] != color) continue;
        int nr = r + dir;
        if (nr < 0 || nr >= HP_SIZE) continue;

        if (board[c][nr] == HP_NONE)
          out[n++] = { (int8_t)c, (int8_t)r, (int8_t)c, (int8_t)nr };
        if (c > 0 && board[c-1][nr] == enemy)
          out[n++] = { (int8_t)c, (int8_t)r, (int8_t)(c-1), (int8_t)nr };
        if (c < HP_SIZE-1 && board[c+1][nr] == enemy)
          out[n++] = { (int8_t)c, (int8_t)r, (int8_t)(c+1), (int8_t)nr };
      }
    }
    return n;
  }

  void checkWin() {
    for (int c = 0; c < HP_SIZE; c++) if (board[c][2] == HP_WHITE) { winner = HP_WHITE; return; }
    for (int c = 0; c < HP_SIZE; c++) if (board[c][0] == HP_BLACK) { winner = HP_BLACK; return; }
    HexapawnMove moves[18];
    if (getValidMoves(HP_WHITE, moves) == 0) { winner = HP_BLACK; return; }
    if (getValidMoves(HP_BLACK, moves) == 0) { winner = HP_WHITE; return; }
    winner = HP_NONE;
  }

  void applyMove(const HexapawnMove& m) {
    board[m.tc][m.tr] = board[m.fc][m.fr];
    board[m.fc][m.fr] = HP_NONE;
    turn = (turn == HP_WHITE) ? HP_BLACK : HP_WHITE;
    checkWin();
  }

  bool isValidMove(int fc, int fr, int tc, int tr) const {
    HexapawnMove moves[18];
    int n = getValidMoves(turn, moves);
    for (int i = 0; i < n; i++) {
      if (moves[i].fc == fc && moves[i].fr == fr &&
          moves[i].tc == tc && moves[i].tr == tr) return true;
    }
    return false;
  }

  // Returns a move that plays optimally for `turn`. Solver is full-depth
  // memoized negamax — hexapawn's game tree is ≤1500 reachable states so
  // this gives perfect play with zero error.
  HexapawnMove computeAiMove() const {
    HexapawnMove moves[18];
    int n = getValidMoves(turn, moves);
    if (n == 0) return {0, 0, 0, 0};

    int bestScore = -1000;
    int bestIdx = 0;
    for (int i = 0; i < n; i++) {
      Hexapawn copy = *this;
      copy.applyMove(moves[i]);
      // Score is from perspective of the side-to-move in `copy`, i.e. the
      // opponent. Negate to bring back to our perspective.
      int score = -solveNegamax(copy, -1000, 1000);
      if (score > bestScore) { bestScore = score; bestIdx = i; }
    }
    return moves[bestIdx];
  }

  // Exposes the raw optimal score (+100 win / -100 loss for side to move).
  // Useful for tests.
  int optimalScore() const {
    return solveNegamax(*this, -1000, 1000);
  }

  // Returns every move that achieves the optimal score for `turn`. For
  // hexapawn several moves can tie; exposing them lets callers pick any.
  int computeAllOptimalMoves(HexapawnMove* out, int maxMoves = 18) const {
    HexapawnMove moves[18];
    int n = getValidMoves(turn, moves);
    if (n == 0) return 0;

    int scores[18];
    int bestScore = -1000;
    for (int i = 0; i < n; i++) {
      Hexapawn copy = *this;
      copy.applyMove(moves[i]);
      int score = -solveNegamax(copy, -1000, 1000);
      scores[i] = score;
      if (score > bestScore) bestScore = score;
    }
    int k = 0;
    for (int i = 0; i < n && k < maxMoves; i++) {
      if (scores[i] == bestScore) out[k++] = moves[i];
    }
    return k;
  }

  static uint8_t toGrid(int8_t g) { return (uint8_t)(g * 3); }
  static int8_t toGame(uint8_t g) { return (int8_t)(g / 3); }

private:
  // ── Memoization table for the negamax solver ──────────────────
  // Hexapawn has ~1500 reachable states, so a tiny open-addressed
  // hash table is plenty. Entries are packed into uint32_t:
  //   bits 0..7  : score + 128  (biased so 0 = empty sentinel works)
  //   bits 8..31 : packed state key
  static constexpr int MEMO_BITS = 12;
  static constexpr int MEMO_SIZE = 1 << MEMO_BITS;
  static constexpr int MEMO_MASK = MEMO_SIZE - 1;
  static uint32_t& memoEntry(uint32_t key) {
    static uint32_t memo[MEMO_SIZE] = {};
    // Probe up to 8 slots. If full, skip caching (correctness unaffected).
    uint32_t idx = key & MEMO_MASK;
    for (int p = 0; p < 8; p++) {
      uint32_t& e = memo[(idx + p) & MEMO_MASK];
      if (e == 0) return e;
      if ((e >> 8) == key) return e;
    }
    static uint32_t sink = 0;  // overflow — recompute each time
    sink = 0;
    return sink;
  }

  static uint32_t packState(const Hexapawn& s) {
    uint32_t k = 0;
    for (int c = 0; c < HP_SIZE; c++)
      for (int r = 0; r < HP_SIZE; r++)
        k |= ((uint32_t)(uint8_t)s.board[c][r]) << ((c*3 + r) * 2);
    k |= ((uint32_t)s.turn) << 18;
    return k;
  }

  // Negamax with alpha-beta. Score is from POV of side-to-move in `s`.
  // +100 = winning, -100 = losing. Full depth — no heuristic fallback.
  static int solveNegamax(const Hexapawn& s, int alpha, int beta) {
    // Terminal positions
    if (s.winner == HP_WHITE) return (s.turn == HP_WHITE) ? 100 : -100;
    if (s.winner == HP_BLACK) return (s.turn == HP_BLACK) ? 100 : -100;

    uint32_t key = packState(s);
    uint32_t& slot = memoEntry(key);
    if (slot != 0 && (slot >> 8) == key) {
      return (int)(slot & 0xFF) - 128;
    }

    HexapawnMove moves[18];
    int n = s.getValidMoves(s.turn, moves);
    if (n == 0) return -100;  // no moves = loss for side to move

    int best = -1000;
    for (int i = 0; i < n; i++) {
      Hexapawn c = s;
      c.applyMove(moves[i]);
      int score = -solveNegamax(c, -beta, -alpha);
      if (score > best) best = score;
      if (best > alpha) alpha = best;
      if (alpha >= beta) break;
    }

    // Cache result. Bias score by +128 to fit in a byte and avoid a zero
    // payload colliding with the "empty slot" sentinel.
    slot = (key << 8) | (uint32_t)(uint8_t)(best + 128);
    return best;
  }
};
