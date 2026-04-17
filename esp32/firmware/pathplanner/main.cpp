// Local test harness for path planner.
//
// Problem file format:
//   - N lines of M chars (top = highest y, bottom = y=0)
//   - '.' = empty, 'p' = black pawn, 'P' = white pawn
//   - One or more "fromX fromY toX toY" lines — solved simultaneously
//
// Example:
//   p.p
//   ...
//   P.P
//   0 0 1 1
//   2 0 2 1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../pathplanner.h"

static const char* status_str(PlanResult::Status s) {
  switch (s) {
    case PlanResult::OK:           return "OK";
    case PlanResult::NO_PIECE:     return "NO_PIECE";
    case PlanResult::OUT_OF_BOUNDS:return "OUT_OF_BOUNDS";
    case PlanResult::NO_PATH:      return "NO_PATH";
    default:                       return "UNKNOWN";
  }
}

static void print_board(uint8_t board[][PP_MAX_ROWS], uint8_t cols, uint8_t rows,
                        const MoveGoal* goals = nullptr, int num_goals = 0,
                        const PlanResult* plan = nullptr,
                        const uint8_t* init_board = nullptr) {
  // Build overlay from all timesteps
  char overlay[PP_MAX_COLS][PP_MAX_ROWS];
  memset(overlay, 0, sizeof(overlay));

  if (plan && goals) {
    // Track each goal piece through timesteps to draw its path
    uint8_t gx[PP_MAX_GOALS], gy[PP_MAX_GOALS];
    for (int i = 0; i < num_goals; i++) {
      gx[i] = goals[i].fromX;
      gy[i] = goals[i].fromY;
      overlay[gx[i]][gy[i]] = 'A' + i;
    }
    for (int t = 0; t < plan->num_steps; t++) {
      auto& ts = plan->steps[t];
      for (int m = 0; m < ts.num_moves; m++) {
        auto& mv = ts.moves[m];
        for (int i = 0; i < num_goals; i++) {
          if (gx[i] == mv.fromX && gy[i] == mv.fromY) {
            gx[i] = mv.toX;
            gy[i] = mv.toY;
            if (!overlay[mv.toX][mv.toY])
              overlay[mv.toX][mv.toY] = '#';
            break;
          }
        }
      }
    }
    // Mark destinations last (overwrite path)
    for (int i = 0; i < num_goals; i++)
      overlay[goals[i].toX][goals[i].toY] = 'a' + i;
  } else if (goals) {
    for (int i = 0; i < num_goals; i++) {
      overlay[goals[i].fromX][goals[i].fromY] = 'A' + i;
      overlay[goals[i].toX][goals[i].toY] = 'a' + i;
    }
  }

  for (int y = rows - 1; y >= 0; y--) {
    printf("  %2d | ", y);
    for (int x = 0; x < cols; x++) {
      uint8_t piece = init_board ? init_board[x * rows + y] : board[x][y];
      char ov = overlay[x][y];
      if (ov >= 'A' && ov < 'A' + PP_MAX_GOALS)
        printf("%c ", ov);
      else if (ov >= 'a' && ov < 'a' + PP_MAX_GOALS)
        printf("%c ", ov);
      else if (ov == '#')
        printf("# ");
      else {
        switch (piece) {
          case PP_WHITE: printf("P "); break;
          case PP_BLACK: printf("p "); break;
          default:       printf(". "); break;
        }
      }
    }
    printf("\n");
  }
  printf("     +");
  for (int x = 0; x < cols; x++) printf("--");
  printf("\n       ");
  for (int x = 0; x < cols; x++) printf("%d ", x);
  printf("\n");
  fflush(stdout);
}

static bool is_board_line(const char* line) {
  if (!line[0] || line[0] == '\n') return false;
  for (int i = 0; line[i] && line[i] != '\n'; i++) {
    if (line[i] != '.' && line[i] != 'P' && line[i] != 'p') return false;
  }
  return true;
}

int main(int argc, char* argv[]) {
  const char* path = nullptr;
  uint8_t max_concurrent = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
      max_concurrent = (uint8_t)atoi(argv[++i]);
    } else if (!path) {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr, "Usage: %s <problem.txt> [--max N]\n", argv[0]);
    return 1;
  }

  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Cannot open %s\n", path);
    return 1;
  }

  char board_lines[PP_MAX_ROWS][256];
  int num_lines = 0;
  uint8_t cols = 0;
  char line[256];

  while (fgets(line, sizeof(line), f)) {
    if (is_board_line(line)) {
      int len = 0;
      while (line[len] && line[len] != '\n') len++;
      if (len > cols) cols = len;
      strncpy(board_lines[num_lines], line, sizeof(board_lines[0]));
      num_lines++;
      if (num_lines >= PP_MAX_ROWS) break;
    } else {
      break;
    }
  }

  uint8_t rows = num_lines;
  if (rows == 0 || cols == 0) {
    fprintf(stderr, "No board found in file\n");
    fclose(f);
    return 1;
  }

  uint8_t board[PP_MAX_COLS][PP_MAX_ROWS];
  memset(board, PP_NONE, sizeof(board));

  for (int row = 0; row < rows; row++) {
    int y = rows - 1 - row;
    for (int x = 0; x < cols && board_lines[row][x] && board_lines[row][x] != '\n'; x++) {
      switch (board_lines[row][x]) {
        case 'P': board[x][y] = PP_WHITE; break;
        case 'p': board[x][y] = PP_BLACK; break;
        default:  board[x][y] = PP_NONE;  break;
      }
    }
  }

  MoveGoal goals[PP_MAX_GOALS];
  int num_goals = 0;

  // Parse move lines: "fromX fromY toX toY" where x means wildcard
  auto parse_goal = [&](const char* s) -> bool {
    char sx[16], sy[16], dx[16], dy[16];
    if (sscanf(s, "%15s %15s %15s %15s", sx, sy, dx, dy) != 4) return false;
    // from must be numeric
    int fxv, fyv;
    if (sscanf(sx, "%d", &fxv) != 1 || sscanf(sy, "%d", &fyv) != 1) return false;
    uint8_t txv = (dx[0] == 'x' || dx[0] == 'X') ? PP_ANY : (uint8_t)atoi(dx);
    uint8_t tyv = (dy[0] == 'x' || dy[0] == 'X') ? PP_ANY : (uint8_t)atoi(dy);
    if (num_goals < PP_MAX_GOALS)
      goals[num_goals++] = {(uint8_t)fxv, (uint8_t)fyv, txv, tyv};
    return true;
  };

  // Try current line and all remaining lines (skip blanks/headers)
  parse_goal(line);
  while (fgets(line, sizeof(line), f))
    parse_goal(line);  // silently skips unparseable lines
  fclose(f);

  if (num_goals == 0) {
    fprintf(stderr, "No move lines found (expected: fromX fromY toX toY)\n");
    return 1;
  }

  auto fmt_coord = [](uint8_t v, char* buf) {
    if (v == PP_ANY) { buf[0] = 'x'; buf[1] = 0; }
    else snprintf(buf, 8, "%d", v);
  };

  printf("Board: %d x %d, %d goal(s)\n", cols, rows, num_goals);
  for (int i = 0; i < num_goals; i++) {
    char tx[8], ty[8];
    fmt_coord(goals[i].toX, tx);
    fmt_coord(goals[i].toY, ty);
    printf("  %c: (%d,%d) -> %c: (%s,%s)\n",
           'A' + i, goals[i].fromX, goals[i].fromY, 'a' + i, tx, ty);
  }
  printf("\n");
  print_board(board, cols, rows, goals, num_goals);
  printf("\n");

  uint8_t init_board[PP_MAX_COLS * PP_MAX_ROWS];
  for (int x = 0; x < cols; x++)
    for (int y = 0; y < rows; y++)
      init_board[x * rows + y] = board[x][y];

  // planPath resolves wildcards in-place on goals
  if (max_concurrent > 0) printf("  (max_concurrent_moves = %u)\n", max_concurrent);
  PlanResult result = planPath(board, cols, rows, goals, num_goals, max_concurrent);

  // Compute cost: orthogonal=10, diagonal=20
  int total_cost = 0;
  for (int t = 0; t < result.num_steps; t++) {
    auto& ts = result.steps[t];
    for (int m = 0; m < ts.num_moves; m++) {
      auto& mv = ts.moves[m];
      bool diag = (mv.fromX != mv.toX) && (mv.fromY != mv.toY);
      total_cost += diag ? 20 : 10;
    }
  }

  printf("Result: %s (%d timesteps, cost %d)\n", status_str(result.status), result.num_steps, total_cost);
  if (result.status == PlanResult::OK) {
    printf("Resolved goals:\n");
    for (int i = 0; i < num_goals; i++)
      printf("  %c: (%d,%d) -> %c: (%d,%d)\n",
             'A' + i, goals[i].fromX, goals[i].fromY,
             'a' + i, goals[i].toX, goals[i].toY);
  }
  printf("\n");

  // Simulate board state at each timestep using piece IDs 1..num_goals
  // Build a separate ID grid (0=empty, 1..N=piece)
  uint8_t id_grid[PP_MAX_COLS][PP_MAX_ROWS];
  memset(id_grid, 0, sizeof(id_grid));
  // Place goal pieces with their IDs
  uint8_t gx[PP_MAX_GOALS], gy[PP_MAX_GOALS];
  for (int i = 0; i < num_goals; i++) {
    gx[i] = goals[i].fromX;
    gy[i] = goals[i].fromY;
    id_grid[gx[i]][gy[i]] = i + 1;
  }
  // Place static pieces (non-goal) as 'S' marker (255)
  for (int x = 0; x < cols; x++)
    for (int y = 0; y < rows; y++)
      if (init_board[x * rows + y] != PP_NONE && !id_grid[x][y])
        id_grid[x][y] = 255;

  // Print initial state
  printf("  t=0 (initial)\n");
  for (int y = rows - 1; y >= 0; y--) {
    printf("  %2d | ", y);
    for (int x = 0; x < cols; x++) {
      uint8_t id = id_grid[x][y];
      if (id == 255)    printf("x ");
      else if (id > 0)  printf("%d ", id);
      else              printf(". ");
    }
    printf("\n");
  }
  printf("     +");
  for (int x = 0; x < cols; x++) printf("--");
  printf("\n\n");

  for (int t = 0; t < result.num_steps; t++) {
    auto& ts = result.steps[t];

    // Print move descriptions
    printf("  t=%d:", t + 1);
    for (int m = 0; m < ts.num_moves; m++) {
      auto& mv = ts.moves[m];
      uint8_t id = id_grid[mv.fromX][mv.fromY];
      if (id == 255)
        printf("  x:(%d,%d)->(%d,%d)", mv.fromX, mv.fromY, mv.toX, mv.toY);
      else
        printf("  %d:(%d,%d)->(%d,%d)", id, mv.fromX, mv.fromY, mv.toX, mv.toY);
    }
    printf("\n");

    // Snapshot which piece ID is at each move's source
    uint8_t move_ids[PP_MAX_GOALS];
    for (int m = 0; m < ts.num_moves; m++)
      move_ids[m] = id_grid[ts.moves[m].fromX][ts.moves[m].fromY];

    // Remove all moving pieces, then place at new positions
    for (int m = 0; m < ts.num_moves; m++)
      id_grid[ts.moves[m].fromX][ts.moves[m].fromY] = 0;
    for (int m = 0; m < ts.num_moves; m++)
      id_grid[ts.moves[m].toX][ts.moves[m].toY] = move_ids[m];

    // Print board
    for (int y = rows - 1; y >= 0; y--) {
      printf("  %2d | ", y);
      for (int x = 0; x < cols; x++) {
        uint8_t id = id_grid[x][y];
        if (id == 255)    printf("x ");
        else if (id > 0)  printf("%d ", id);
        else              printf(". ");
      }
      printf("\n");
    }
    printf("     +");
    for (int x = 0; x < cols; x++) printf("--");
    printf("\n\n");
  }
  fflush(stdout);

  return (result.status == PlanResult::OK) ? 0 : 1;
}
