// Board is the game-logic layer. It must NOT directly manipulate shift registers,
// OE, or PWM. All coil actuation goes through Hardware's safe public API:
//   pulseBit()      — fixed-duration pulse with thermal protection
//   sustainCoil()   — sustain active coil without SPI writes
// Hardware encapsulates all dangerous SR/OE/PWM operations as private.

#pragma once
#include <Preferences.h>
#include "api.h"
#include "hardware.h"
#include "physics.h"
#include "hexapawn.h"

// Piece IDs
#define PIECE_NONE  0
#define PIECE_WHITE 1
#define PIECE_BLACK 2

// Move parameters
#define MOVE_PULSE_MS       350
#define MOVE_DELAY_MS       0
#define MOVE_PULSE_REPEATS  1

class Board {
public:
  using PollCallback = void(*)();
  PollCallback poll_callback_ = nullptr;

  void setPollCallback(PollCallback cb) { poll_callback_ = cb; }

  Board() {
    initDefaultBoard();
    loadPhysicsParams();
  }

  // ── Piece State ─────────────────────────────────────────────

  uint8_t getPiece(uint8_t x, uint8_t y) const {
    if (x >= GRID_COLS || y >= GRID_ROWS) return PIECE_NONE;
    return pieces_[x][y];
  }

  void setPiece(uint8_t x, uint8_t y, uint8_t id) {
    if (x >= GRID_COLS || y >= GRID_ROWS) return;
    pieces_[x][y] = id;
  }

  // ── Dumb Orthogonal Move ────────────────────────────────────
  // Validates the move, then pulses coils along the path.

  MoveError moveDumbOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY, bool skipValidation = false) {
    LOG_BOARD("moveDumbOrthogonal: (%d,%d) -> (%d,%d) skip=%d", fromX, fromY, toX, toY, skipValidation);

    // Bounds check always applies
    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS) {
      return MoveError::OUT_OF_BOUNDS;
    }

    if (fromX == toX && fromY == toY) {
      return MoveError::SAME_POSITION;
    }

    if (fromX != toX && fromY != toY) {
      return MoveError::NOT_ORTHOGONAL;
    }

    if (!skipValidation) {
      if (getPiece(fromX, fromY) == PIECE_NONE) {
        return MoveError::NO_PIECE_AT_SOURCE;
      }

      int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
      int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
      int8_t cx = fromX + dx, cy = fromY + dy;
      while (cx != toX || cy != toY) {
        if (getPiece(cx, cy) != PIECE_NONE) {
          LOG_BOARD("moveDumbOrthogonal REJECT: path blocked at (%d,%d)", cx, cy);
          return MoveError::PATH_BLOCKED;
        }
        cx += dx;
        cy += dy;
      }
    }

    // Pulse coils along the path
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    uint8_t piece = pieces_[fromX][fromY];
    int8_t cx = fromX, cy = fromY;

    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;

      int8_t bit = coordToBit(cx, cy);
      LOG_BOARD("moveDumbOrthogonal: pulsing (%d,%d) bit=%d for %dms", cx, cy, bit, MOVE_PULSE_MS);
      for (int r = 0; r < MOVE_PULSE_REPEATS; r++) {
        PulseCoilResponse res = pulseCoil(cx, cy, MOVE_PULSE_MS);
        if (!res.success) {
          LOG_BOARD("moveDumbOrthogonal ABORT: pulse failed at (%d,%d), error=%d", cx, cy, (int)res.error);
          return MoveError::COIL_FAILURE;
        }
        if (r < MOVE_PULSE_REPEATS - 1) delay(MOVE_DELAY_MS);
      }
      delay(MOVE_DELAY_MS);
    }

    // Update piece state
    pieces_[fromX][fromY] = PIECE_NONE;
    pieces_[toX][toY] = piece;
    LOG_BOARD("moveDumbOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
    return MoveError::NONE;
  }

  // ── Physics Orthogonal Move ──────────────────────────────────

  MoveError movePhysicsOrthogonal(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                  bool skipValidation = false, int max_retry_attempts = -1,
                                  MoveDiag* out_diag = nullptr) {
    const PhysicsParams& params = physics_params_;
    if (max_retry_attempts < 0) max_retry_attempts = params.max_retry_attempts;
    LOG_BOARD("movePhysicsOrthogonal: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    if (fromX >= GRID_COLS || fromY >= GRID_ROWS || toX >= GRID_COLS || toY >= GRID_ROWS)
      return MoveError::OUT_OF_BOUNDS;
    if (fromX == toX && fromY == toY) return MoveError::SAME_POSITION;
    if (fromX != toX && fromY != toY) return MoveError::NOT_ORTHOGONAL;

    if (!skipValidation) {
      if (getPiece(fromX, fromY) == PIECE_NONE) return MoveError::NO_PIECE_AT_SOURCE;
      int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
      int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
      int8_t cx = fromX + dx, cy = fromY + dy;
      while (cx != toX || cy != toY) {
        if (getPiece(cx, cy) != PIECE_NONE) return MoveError::PATH_BLOCKED;
        cx += dx; cy += dy;
      }
    }

    // Build coil path in mm
    int8_t stepX = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t stepY = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    float path_mm[GRID_COLS + GRID_ROWS][2];
    int path_len = 0;
    int8_t cx = fromX, cy = fromY;
    while (cx != toX || cy != toY) {
      cx += stepX;
      cy += stepY;
      path_mm[path_len][0] = cx * GRID_TO_MM;
      path_mm[path_len][1] = cy * GRID_TO_MM;
      path_len++;
    }

    // Build sensor list — only at major grid positions (multiples of SR_BLOCK)
    // where actual Hall sensors exist. Skip duplicates.
    uint8_t path_sensors[MAX_DIAG_COILS];
    float sensor_thresholds[MAX_DIAG_COILS];
    int num_path_sensors = 0;
    {
      uint8_t last_si = sensorForGrid(fromX, fromY);  // source sensor (don't include)
      int8_t sx = fromX, sy = fromY;
      for (int i = 0; i < path_len && num_path_sensors < MAX_DIAG_COILS; i++) {
        sx += stepX; sy += stepY;
        // Only add sensor at major grid positions (where sensors physically exist)
        if (sx % SR_BLOCK != 0 || sy % SR_BLOCK != 0) continue;
        uint8_t si = sensorForGrid(sx, sy);
        if (si == last_si) continue;  // skip duplicate
        float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2040.0f;
        float piece_mean = cal_data_.valid ? cal_data_.sensors[si].piece_mean : 1860.0f;
        path_sensors[num_path_sensors] = si;
        sensor_thresholds[num_path_sensors] = (baseline + piece_mean) / 2.0f;
        last_si = si;
        num_path_sensors++;
      }
    }

    MoveDiag diag;

    // Execute physics move with diagnostics
    PieceState& ps = piece_states_[fromX][fromY];
    ps.reset(fromX * GRID_TO_MM, fromY * GRID_TO_MM);
    MoveError err = physics_.execute(ps, path_mm, path_len, params,
                                     &diag, path_sensors, num_path_sensors, sensor_thresholds);

    // Checkpoint: verify destination sensor detects the piece
    if (num_path_sensors > 0) {
      int dest_idx = num_path_sensors - 1;
      delay(50);  // settle time
      uint16_t dest_reading = hw_.readSensor(path_sensors[dest_idx]);
      diag.coils[dest_idx].arrival_reading = dest_reading;
      diag.checkpoint_ok = (dest_reading < sensor_thresholds[dest_idx]);
    } else {
      diag.checkpoint_ok = true;  // no sensors, assume OK
    }

    // Log diagnostic summary
    {
      String log = "physics: diag";
      for (int i = 0; i < diag.num_coils; i++) {
        log += " coil" + String(i) + "=" + (diag.coils[i].detected ? "OK" : "MISS");
        log += "(" + String(diag.coils[i].min_reading) + ")";
      }
      log += " checkpoint=" + String(diag.checkpoint_ok ? "OK" : "FAIL");
      LOG_BOARD("%s", log.c_str());
    }

    if (err == MoveError::NONE && diag.checkpoint_ok) {
      // Success — update board state
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[toX][toY] = piece;
      piece_states_[toX][toY] = ps;
      LOG_BOARD("movePhysicsOrthogonal OK: piece %d now at (%d,%d)", piece, toX, toY);
      if (out_diag) *out_diag = diag;
      return MoveError::NONE;
    }

    // Checkpoint failed — attempt recovery if retries remain
    if (!diag.checkpoint_ok && max_retry_attempts > 0) {
      LOG_BOARD("physics: checkpoint FAIL, attempting recovery (retries left: %d)", max_retry_attempts);

      // Find last coil that detected the piece (scan backwards from destination)
      int last_ok = -1;
      for (int i = diag.num_coils - 1; i >= 0; i--) {
        if (diag.coils[i].detected) { last_ok = i; break; }
      }

      // Recovery target: the coil after last_ok (or the first coil if none detected)
      int recovery_idx = (last_ok >= 0) ? last_ok + 1 : 0;
      if (recovery_idx >= path_len) {
        // All coils detected but checkpoint still failed — piece overshot?
        LOG_BOARD("physics: all coils detected but checkpoint failed, no recovery possible");
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return err != MoveError::NONE ? err : MoveError::COIL_FAILURE;
      }

      // Compute grid position of recovery target
      uint8_t recX = fromX + stepX * (recovery_idx + 1);
      uint8_t recY = fromY + stepY * (recovery_idx + 1);

      // Compute grid position of last-ok coil (or source if none detected)
      uint8_t lastX = (last_ok >= 0) ? fromX + stepX * (last_ok + 1) : fromX;
      uint8_t lastY = (last_ok >= 0) ? fromY + stepY * (last_ok + 1) : fromY;

      LOG_BOARD("physics: recovery: last detected=(%d,%d), pushing to (%d,%d) via moveDumb",
                lastX, lastY, recX, recY);

      // Update board state: piece is at last-ok position
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[lastX][lastY] = piece;

      // Push piece to recovery target with moveDumb
      MoveError dumbErr = moveDumbOrthogonal(lastX, lastY, recX, recY, true);
      if (dumbErr != MoveError::NONE) {
        LOG_BOARD("physics: recovery moveDumb failed: %d", (int)dumbErr);
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return dumbErr;
      }

      // Verify piece arrived at recovery target
      uint8_t recSensor = sensorForGrid(recX, recY);
      delay(50);
      uint16_t recReading = hw_.readSensor(recSensor);
      float recBaseline = cal_data_.valid ? cal_data_.sensors[recSensor].baseline_mean : 2040.0f;
      float recPieceMean = cal_data_.valid ? cal_data_.sensors[recSensor].piece_mean : 1860.0f;
      float recThreshold = (recBaseline + recPieceMean) / 2.0f;

      if (recReading >= recThreshold) {
        LOG_BOARD("physics: recovery verification FAILED at (%d,%d) reading=%d threshold=%.0f",
                  recX, recY, recReading, recThreshold);
        if (out_diag) { diag.retries_used++; *out_diag = diag; }
        return MoveError::COIL_FAILURE;
      }

      LOG_BOARD("physics: recovery verified at (%d,%d), retrying physics to (%d,%d)",
                recX, recY, toX, toY);

      // Recurse: physics move from recovery position to original destination
      diag.retries_used++;
      MoveDiag retry_diag;
      MoveError retryErr = movePhysicsOrthogonal(recX, recY, toX, toY,
                                                  true, max_retry_attempts - 1, &retry_diag);
      retry_diag.retries_used += diag.retries_used;
      if (out_diag) *out_diag = retry_diag;
      return retryErr;
    }

    // No recovery — return result with diag
    if (err == MoveError::NONE) {
      // Physics said OK but checkpoint failed and no retries — still update board state
      uint8_t piece = pieces_[fromX][fromY];
      pieces_[fromX][fromY] = PIECE_NONE;
      pieces_[toX][toY] = piece;
      piece_states_[toX][toY] = ps;
      LOG_BOARD("movePhysicsOrthogonal: physics OK but checkpoint FAIL, piece %d at (%d,%d)", piece, toX, toY);
    } else {
      LOG_BOARD("movePhysicsOrthogonal FAILED: %d", (int)err);
    }
    if (out_diag) *out_diag = diag;
    return err;
  }

  // ── High-Level Move Planner ─────────────────────────────────
  // Handles path planning, captures, and obstacle clearing.

  bool use_physics_moves_ = true;

  void setUsePhysics(bool val) { use_physics_moves_ = val; }

  // ── Hexapawn Game (autonomous) ──────────────────────────────
  // Runs the entire game on the ESP32. Streams progress via LOG_BOARD.
  // White = human (detected via sensors), Black = AI (moved via coils).

  Hexapawn game_;

  // Sensor detection: does sensor at game col,row detect a piece?
  bool sensorDetectsPiece(int gc, int gr) {
    uint8_t si = sensorForGrid(Hexapawn::toGrid(gc), Hexapawn::toGrid(gr));
    uint16_t reading = hw_.readSensor(si);
    float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2040.0f;
    float piece_mean = cal_data_.valid ? cal_data_.sensors[si].piece_mean : 1860.0f;
    float threshold = (baseline + piece_mean) / 2.0f;
    return reading < threshold;
  }

  String hexapawnPlay(uint16_t hint_pulse_ms = 0) {
    game_.reset();
    initDefaultBoard();

    LOG_BOARD("hexapawn: === NEW GAME ===");

    // Center all starting pieces with a pulse on each position
    if (hint_pulse_ms > 0) {
      LOG_BOARD("hexapawn: centering pieces...");
      for (int c = 0; c < HP_SIZE; c++) {
        // White row (row 0)
        uint8_t gx = Hexapawn::toGrid(c), gy = Hexapawn::toGrid(0);
        int8_t bit = coordToBit(gx, gy);
        if (bit >= 0) hw_.pulseBit((uint8_t)bit, hint_pulse_ms, 255);
        delay(100);
      }
      for (int c = 0; c < HP_SIZE; c++) {
        // Black row (row 2)
        uint8_t gx = Hexapawn::toGrid(c), gy = Hexapawn::toGrid(2);
        int8_t bit = coordToBit(gx, gy);
        if (bit >= 0) hw_.pulseBit((uint8_t)bit, hint_pulse_ms, 255);
        delay(100);
      }
    }

    // Verify all 6 pieces are on the board
    int detected = 0;
    for (int c = 0; c < HP_SIZE; c++) {
      if (sensorDetectsPiece(c, 0)) detected++;
      if (sensorDetectsPiece(c, 2)) detected++;
    }
    if (detected < 6) {
      LOG_BOARD("hexapawn: ERROR — only %d/6 pieces detected", detected);
      return Json().add("success", false)
                   .addStr("error", "not all pieces detected")
                   .add("detected", detected).build();
    }
    LOG_BOARD("hexapawn: all 6 pieces detected, game starting");

    int move_num = 0;
    while (game_.winner == HP_NONE) {
      move_num++;

      if (game_.turn == HP_WHITE) {
        // ── Human turn ──
        LOG_BOARD("hexapawn: [move %d] your turn (White)", move_num);

        // Get all valid moves for white
        HexapawnMove allMoves[18];
        int nMoves = game_.getValidMoves(HP_WHITE, allMoves);

        // Determine which moves are captures
        bool captureAvail = false;
        for (int i = 0; i < nMoves; i++) {
          if (game_.board[allMoves[i].tc][allMoves[i].tr] == HP_BLACK) {
            captureAvail = true;
            break;
          }
        }

        // Snapshot current sensor state for all white pieces
        bool whiteSensor[HP_SIZE][HP_SIZE] = {};
        bool blackSensor[HP_SIZE][HP_SIZE] = {};
        for (int c = 0; c < HP_SIZE; c++) {
          for (int r = 0; r < HP_SIZE; r++) {
            if (game_.board[c][r] == HP_WHITE) whiteSensor[c][r] = sensorDetectsPiece(c, r);
            if (game_.board[c][r] == HP_BLACK) blackSensor[c][r] = sensorDetectsPiece(c, r);
          }
        }

        // Wait for a change — either a white piece lifted or an enemy piece removed
        int lifted_c = -1, lifted_r = -1;
        int captured_c = -1, captured_r = -1;
        bool capturePhase = false;

        unsigned long t0 = millis();
        while (millis() - t0 < 120000) {  // 2 min timeout per turn
          if (poll_callback_) poll_callback_();
          delay(200);

          if (!capturePhase) {
            // Phase 1: detect a piece being lifted or removed
            // Check if an enemy piece was removed (capture start)
            for (int c = 0; c < HP_SIZE; c++) {
              for (int r = 0; r < HP_SIZE; r++) {
                if (game_.board[c][r] == HP_BLACK && blackSensor[c][r] && !sensorDetectsPiece(c, r)) {
                  // Enemy piece lifted — this is a capture
                  captured_c = c; captured_r = r;
                  LOG_BOARD("hexapawn: enemy piece removed from (%d,%d)", c, r);
                  LOG_BOARD("hexapawn: place it in the graveyard, then move your piece there");
                  capturePhase = true;
                  // Update both firmware and game board state
                  pieces_[Hexapawn::toGrid(c)][Hexapawn::toGrid(r)] = PIECE_NONE;
                  game_.board[c][r] = HP_NONE;
                  break;
                }
              }
              if (capturePhase) break;
            }

            if (!capturePhase) {
              // Check if a white piece was lifted (normal move start)
              for (int c = 0; c < HP_SIZE; c++) {
                for (int r = 0; r < HP_SIZE; r++) {
                  if (game_.board[c][r] == HP_WHITE && whiteSensor[c][r] && !sensorDetectsPiece(c, r)) {
                    // Check this piece has valid non-capture moves
                    bool hasMove = false;
                    for (int i = 0; i < nMoves; i++) {
                      if (allMoves[i].fc == c && allMoves[i].fr == r &&
                          game_.board[allMoves[i].tc][allMoves[i].tr] == HP_NONE) {
                        hasMove = true;
                        break;
                      }
                    }
                    if (hasMove) {
                      lifted_c = c; lifted_r = r;
                      LOG_BOARD("hexapawn: white piece lifted from (%d,%d)", c, r);
                    }
                  }
                }
              }
            }
          }

          if (capturePhase && lifted_c < 0) {
            // Capture phase: wait for the white piece to be lifted
            for (int c = 0; c < HP_SIZE; c++) {
              for (int r = 0; r < HP_SIZE; r++) {
                if (game_.board[c][r] == HP_WHITE && whiteSensor[c][r] && !sensorDetectsPiece(c, r)) {
                  // Check this piece can capture the removed enemy position
                  for (int i = 0; i < nMoves; i++) {
                    if (allMoves[i].fc == c && allMoves[i].fr == r &&
                        allMoves[i].tc == captured_c && allMoves[i].tr == captured_r) {
                      lifted_c = c; lifted_r = r;
                      LOG_BOARD("hexapawn: white piece lifted from (%d,%d) for capture", c, r);
                      break;
                    }
                  }
                }
              }
            }
          }

          // Wait for placement: white piece appears at valid destination
          if (lifted_c >= 0) {
            for (int i = 0; i < nMoves; i++) {
              if (allMoves[i].fc != lifted_c || allMoves[i].fr != lifted_r) continue;
              int dc = allMoves[i].tc, dr = allMoves[i].tr;

              // For capture: destination is where enemy was (already empty)
              // For normal: destination must be empty
              if (dc == lifted_c && dr == lifted_r) continue;  // can't place back
              if (sensorDetectsPiece(dc, dr)) {
                // Piece detected at valid destination
                LOG_BOARD("hexapawn: piece placed at (%d,%d)", dc, dr);

                // Apply move to game state
                bool isCapture = (captured_c == dc && captured_r == dr);
                HexapawnMove pm = { (int8_t)lifted_c, (int8_t)lifted_r, (int8_t)dc, (int8_t)dr };
                game_.applyMove(pm);

                // Update firmware board state
                pieces_[Hexapawn::toGrid(lifted_c)][Hexapawn::toGrid(lifted_r)] = PIECE_NONE;
                pieces_[Hexapawn::toGrid(dc)][Hexapawn::toGrid(dr)] = PIECE_WHITE;

                LOG_BOARD("hexapawn: White (%d,%d)->(%d,%d)%s",
                          lifted_c, lifted_r, dc, dr, isCapture ? " CAPTURE" : "");
                goto turn_done;
              }
            }

            // Check if piece was put back (not a move, reset)
            if (sensorDetectsPiece(lifted_c, lifted_r) && !capturePhase) {
              LOG_BOARD("hexapawn: piece returned to (%d,%d), still your turn", lifted_c, lifted_r);
              lifted_c = -1; lifted_r = -1;
            }

            // Hint: pulse source square + valid destination coils every ~1s
            if (hint_pulse_ms > 0 && (millis() / 1000 != (millis() - 200) / 1000)) {
              // Pulse source square (return-to + re-center)
              {
                uint8_t gx = Hexapawn::toGrid(lifted_c), gy = Hexapawn::toGrid(lifted_r);
                int8_t bit = coordToBit(gx, gy);
                if (bit >= 0) hw_.pulseBit((uint8_t)bit, hint_pulse_ms, 255);
              }
              // Pulse valid destinations
              for (int i = 0; i < nMoves; i++) {
                if (allMoves[i].fc != lifted_c || allMoves[i].fr != lifted_r) continue;
                int dc = allMoves[i].tc, dr = allMoves[i].tr;
                uint8_t gx = Hexapawn::toGrid(dc), gy = Hexapawn::toGrid(dr);
                int8_t bit = coordToBit(gx, gy);
                if (bit >= 0) hw_.pulseBit((uint8_t)bit, hint_pulse_ms, 255);
              }
            }
          }
        }

        // Timeout
        LOG_BOARD("hexapawn: timeout waiting for player");
        return Json().add("success", false).addStr("error", "timeout").build();

        turn_done:;

      } else {
        // ── AI turn ──
        LOG_BOARD("hexapawn: [move %d] AI thinking (Black)...", move_num);
        if (poll_callback_) poll_callback_();
        delay(500);

        HexapawnMove ai = game_.computeAiMove();

        // Check AI has valid moves (shouldn't happen — checkWin catches it, but be safe)
        HexapawnMove aiMoves[18];
        int nAiMoves = game_.getValidMoves(HP_BLACK, aiMoves);
        if (nAiMoves == 0) {
          LOG_BOARD("hexapawn: AI has no valid moves");
          game_.winner = HP_WHITE;
          break;
        }

        bool capture = (game_.board[ai.tc][ai.tr] == HP_WHITE);

        LOG_BOARD("hexapawn: AI moves (%d,%d)->(%d,%d)%s",
                  ai.fc, ai.fr, ai.tc, ai.tr, capture ? " CAPTURE" : "");

        // Execute physical move FIRST (movePiece handles captures + path clearing)
        uint8_t gfx = Hexapawn::toGrid(ai.fc), gfy = Hexapawn::toGrid(ai.fr);
        uint8_t gtx = Hexapawn::toGrid(ai.tc), gty = Hexapawn::toGrid(ai.tr);
        MoveError err = movePiece(gfx, gfy, gtx, gty);
        if (err != MoveError::NONE) {
          LOG_BOARD("hexapawn: AI physical move FAILED: %d — applying to game state anyway", (int)err);
        }

        // Apply to game state AFTER physical execution
        game_.applyMove(ai);

        if (poll_callback_) poll_callback_();
      }

      if (game_.winner != HP_NONE) {
        const char* w = (game_.winner == HP_WHITE) ? "White (You)" : "Black (AI)";
        LOG_BOARD("hexapawn: === %s WINS! === (move %d)", w, move_num);
      }
    }

    return Json().add("success", true)
                 .addStr("winner", game_.winner == HP_WHITE ? "white" : "black")
                 .add("moves", move_num).build();
  }

  MoveError movePiece(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                      int max_retry_attempts = -1) {
    LOG_BOARD("movePiece: (%d,%d) -> (%d,%d)", fromX, fromY, toX, toY);

    if (getPiece(fromX, fromY) == PIECE_NONE) return MoveError::NO_PIECE_AT_SOURCE;

    // Handle capture: if destination has an enemy piece, kill it first
    uint8_t destPiece = getPiece(toX, toY);
    if (destPiece != PIECE_NONE && destPiece != getPiece(fromX, fromY)) {
      MoveError err = killPiece(toX, toY);
      if (err != MoveError::NONE) return err;
    }

    // Manhattan routing: if diagonal, prefer path with no obstructions.
    // Only use moveOrthogonalClearing (which displaces pieces) as last resort.
    if (fromX != toX && fromY != toY) {
      int obstX1st = countObstructions(fromX, fromY, toX, fromY)
                   + countObstructions(toX, fromY, toX, toY);
      int obstY1st = countObstructions(fromX, fromY, fromX, toY)
                   + countObstructions(fromX, toY, toX, toY);

      // Try clear path first (no obstructions), fall back to fewer obstructions
      bool xFirst;
      if (obstX1st == 0 && obstY1st > 0) xFirst = true;
      else if (obstY1st == 0 && obstX1st > 0) xFirst = false;
      else xFirst = (obstX1st <= obstY1st);

      if (xFirst) {
        MoveError err = moveOrthogonalClearing(fromX, fromY, toX, fromY, 0, max_retry_attempts);
        if (err != MoveError::NONE) return err;
        return moveOrthogonalClearing(toX, fromY, toX, toY, 0, max_retry_attempts);
      } else {
        MoveError err = moveOrthogonalClearing(fromX, fromY, fromX, toY, 0, max_retry_attempts);
        if (err != MoveError::NONE) return err;
        return moveOrthogonalClearing(fromX, toY, toX, toY, 0, max_retry_attempts);
      }
    }

    // Simple orthogonal move
    return moveOrthogonalClearing(fromX, fromY, toX, toY, 0, max_retry_attempts);
  }

  // Kill a piece by moving it to the graveyard.
  // Picks the graveyard slot that requires the fewest displacements.
  MoveError killPiece(uint8_t x, uint8_t y) {
    if (getPiece(x, y) == PIECE_NONE) return MoveError::NONE;

    // Try all empty graveyard slots, pick the one with least obstructions
    static const uint8_t slots[] = {0, 3, 6};
    int bestObs = 999;
    int8_t bestSlot = -1;

    for (auto sy : slots) {
      if (getPiece(GRAVE_X, sy) != PIECE_NONE) continue;

      // Count obstructions for both Manhattan orderings, take the minimum
      int obstX1st = countObstructions(x, y, GRAVE_X, y)
                   + countObstructions(GRAVE_X, y, GRAVE_X, sy);
      int obstY1st = countObstructions(x, y, x, sy)
                   + countObstructions(x, sy, GRAVE_X, sy);
      int best = (obstX1st < obstY1st) ? obstX1st : obstY1st;

      if (best < bestObs) {
        bestObs = best;
        bestSlot = sy;
        if (best == 0) break;  // can't do better than zero
      }
    }

    if (bestSlot < 0) {
      LOG_BOARD("killPiece: no graveyard slot available!");
      return MoveError::COIL_FAILURE;
    }

    LOG_BOARD("killPiece: (%d,%d) -> graveyard (%d,%d) obst=%d", x, y, GRAVE_X, bestSlot, bestObs);
    return movePiece(x, y, GRAVE_X, (uint8_t)bestSlot);
  }

private:
  static constexpr uint8_t GRAVE_X = 9;
  static constexpr int MAX_CLEAR_DEPTH = 10;

  // Find an empty graveyard slot (x=9, y=0,3,6)
  int8_t findGraveyardSlot() {
    static const uint8_t slots[] = {0, 3, 6};
    for (auto sy : slots) {
      if (getPiece(GRAVE_X, sy) == PIECE_NONE) return sy;
    }
    return -1;  // graveyard full
  }

  // Count pieces blocking an orthogonal path (exclusive of from, inclusive of to)
  int countObstructions(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    if (fromX == toX && fromY == toY) return 0;
    int count = 0;
    int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    int8_t cx = fromX + dx, cy = fromY + dy;
    while (cx != toX || cy != toY) {
      if (getPiece(cx, cy) != PIECE_NONE) count++;
      cx += dx; cy += dy;
    }
    return count;
  }

  // Execute a single orthogonal atomic move (dumb or physics)
  MoveError atomicMove(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY) {
    if (use_physics_moves_) {
      return movePhysicsOrthogonal(fromX, fromY, toX, toY, true);
    } else {
      return moveDumbOrthogonal(fromX, fromY, toX, toY, true);
    }
  }

  // Move orthogonally, clearing any blocking pieces out of the way
  MoveError moveOrthogonalClearing(uint8_t fromX, uint8_t fromY, uint8_t toX, uint8_t toY,
                                   int depth = 0, int max_retry_attempts = -1) {
    if (depth > MAX_CLEAR_DEPTH) {
      LOG_BOARD("moveOrthogonalClearing: max depth exceeded");
      return MoveError::COIL_FAILURE;
    }
    if (fromX == toX && fromY == toY) return MoveError::NONE;

    // Must be orthogonal
    if (fromX != toX && fromY != toY) return MoveError::NOT_ORTHOGONAL;

    int8_t dx = (toX > fromX) ? 1 : (toX < fromX) ? -1 : 0;
    int8_t dy = (toY > fromY) ? 1 : (toY < fromY) ? -1 : 0;
    bool horizontal = (dx != 0);

    // Collect blocking pieces along the path (from→to exclusive of from)
    struct Blocker { uint8_t x, y; };
    Blocker blockers[GRID_COLS + GRID_ROWS];
    int nblock = 0;

    int8_t cx = fromX + dx, cy = fromY + dy;
    while (cx != (int8_t)toX + dx || cy != (int8_t)toY + dy) {
      if (getPiece(cx, cy) != PIECE_NONE && (cx != fromX || cy != fromY)) {
        blockers[nblock++] = { (uint8_t)cx, (uint8_t)cy };
      }
      cx += dx; cy += dy;
    }

    // Slide each blocker perpendicular
    struct Displaced { uint8_t ox, oy, nx, ny; };
    Displaced displaced[GRID_COLS + GRID_ROWS];
    int ndisp = 0;

    for (int i = 0; i < nblock; i++) {
      uint8_t bx = blockers[i].x, by = blockers[i].y;

      // Find a perpendicular direction to slide this piece
      // Try both directions, pick the one with fewer obstructions
      int8_t perpDx = horizontal ? 0 : 1;
      int8_t perpDy = horizontal ? 1 : 0;

      // Try positive direction first, then negative (use signed to avoid underflow)
      int16_t tgt1x = (int16_t)bx + perpDx * 3, tgt1y = (int16_t)by + perpDy * 3;
      int16_t tgt2x = (int16_t)bx - perpDx * 3, tgt2y = (int16_t)by - perpDy * 3;

      bool tgt1ok = tgt1x >= 0 && tgt1x < GRID_COLS && tgt1y >= 0 && tgt1y < GRID_ROWS;
      bool tgt2ok = tgt2x >= 0 && tgt2x < GRID_COLS && tgt2y >= 0 && tgt2y < GRID_ROWS;

      int obs1 = tgt1ok ? (getPiece(tgt1x, tgt1y) != PIECE_NONE ? 1 : 0) + countObstructions(bx, by, tgt1x, tgt1y) : 999;
      int obs2 = tgt2ok ? (getPiece(tgt2x, tgt2y) != PIECE_NONE ? 1 : 0) + countObstructions(bx, by, tgt2x, tgt2y) : 999;

      uint8_t tx, ty;
      if (obs1 <= obs2 && tgt1ok) { tx = tgt1x; ty = tgt1y; }
      else if (tgt2ok) { tx = tgt2x; ty = tgt2y; }
      else {
        LOG_BOARD("moveOrthogonalClearing: can't clear (%d,%d)", bx, by);
        // Move displaced pieces back before failing
        for (int j = ndisp - 1; j >= 0; j--) {
          atomicMove(displaced[j].nx, displaced[j].ny, displaced[j].ox, displaced[j].oy);
        }
        return MoveError::PATH_BLOCKED;
      }

      // Recursively clear and move the blocker
      MoveError err = moveOrthogonalClearing(bx, by, tx, ty, depth + 1, max_retry_attempts);
      if (err != MoveError::NONE) {
        // Move displaced pieces back before failing
        for (int j = ndisp - 1; j >= 0; j--) {
          atomicMove(displaced[j].nx, displaced[j].ny, displaced[j].ox, displaced[j].oy);
        }
        return err;
      }
      displaced[ndisp++] = { bx, by, tx, ty };
    }

    // Path is clear — execute the main move
    MoveError err = use_physics_moves_
      ? movePhysicsOrthogonal(fromX, fromY, toX, toY, false, max_retry_attempts)
      : moveDumbOrthogonal(fromX, fromY, toX, toY, true);

    // Move displaced pieces back (in reverse order)
    for (int j = ndisp - 1; j >= 0; j--) {
      // Don't move back pieces that ended up in the graveyard
      if (displaced[j].nx == GRAVE_X) continue;
      atomicMove(displaced[j].nx, displaced[j].ny, displaced[j].ox, displaced[j].oy);
    }

    return err;
  }

public:
  // ── Physics Tuning ────────────────────────────────────────────
  // Automated 4-phase tuning: sensor fit, static friction, force profile, verification.
  // Place piece at (3,3) before calling.

  static constexpr int TUNE_NUM_REPS = 5;
  static constexpr int TUNE_FRICTION_PULSE_MS = 100;
  static constexpr int TUNE_FRICTION_DUTY_STEP = 10;
  static constexpr int TUNE_SETTLE_MS = 50;
  static constexpr uint8_t TUNE_X = 3;
  static constexpr uint8_t TUNE_Y = 3;
  static constexpr int TUNE_DURATIONS[] = {10, 25, 50, 100, 200};
  static constexpr int TUNE_NUM_DURATIONS = 5;

  String tunePhysics() {
    LOG_BOARD("TUNE: start — expecting piece at (%d,%d)", TUNE_X, TUNE_Y);

    memset(pieces_, PIECE_NONE, sizeof(pieces_));
    pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;

    uint8_t si = sensorForGrid(TUNE_X, TUNE_Y);
    int8_t coil_bit = coordToBit(TUNE_X + 1, TUNE_Y);  // adjacent coil for testing

    // Get baseline
    float baseline = cal_data_.valid ? cal_data_.sensors[si].baseline_mean : 2040.0f;

    // ── Phase 1: Sensor curve fitting ──
    LOG_BOARD("TUNE: phase1 - sensor curve fitting");
    uint16_t readings_d0[TUNE_NUM_REPS];
    uint16_t readings_d1[TUNE_NUM_REPS];
    uint16_t readings_d2[TUNE_NUM_REPS];

    // d=0: piece at sensor center
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      readings_d0[i] = hw_.readSensor(si);
      delay(10);
    }

    // d=1: move piece one unit away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 1, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d1[i] = hw_.readSensor(si);
      calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // d=2: move piece two units away
    for (int i = 0; i < TUNE_NUM_REPS; i++) {
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      delay(TUNE_SETTLE_MS);
      readings_d2[i] = hw_.readSensor(si);
      calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
      delay(TUNE_SETTLE_MS);
    }

    // Sort arrays for median
    tuneSort(readings_d0, TUNE_NUM_REPS);
    tuneSort(readings_d1, TUNE_NUM_REPS);
    tuneSort(readings_d2, TUNE_NUM_REPS);

    float str0 = baseline - readings_d0[TUNE_NUM_REPS / 2];
    float str1 = baseline - readings_d1[TUNE_NUM_REPS / 2];
    float str2 = baseline - readings_d2[TUNE_NUM_REPS / 2];

    // Fit sensor_k and sensor_falloff
    float eps = 0.3f;  // use force_epsilon as shared epsilon
    float fitted_sensor_k = (str0 > 0) ? str0 * eps : 500.0f;
    float fitted_sensor_falloff = 2.0f;  // default
    if (str1 > 1.0f && str2 > 1.0f) {
      // Solve: str1/str2 = (2^falloff + eps) / (1 + eps)
      float ratio = str1 / str2;
      // Approximate: try falloff values and pick best fit
      float best_err = 999;
      for (float f = 1.0f; f <= 4.0f; f += 0.1f) {
        float predicted_ratio = (powf(2.0f, f) + eps) / (1.0f + eps);
        float err = fabsf(predicted_ratio - ratio);
        if (err < best_err) { best_err = err; fitted_sensor_falloff = f; }
      }
    }

    LOG_BOARD("TUNE: sensor fit: k=%.1f falloff=%.2f (str: d0=%.0f d1=%.0f d2=%.0f)",
              fitted_sensor_k, fitted_sensor_falloff, str0, str1, str2);

    // ── Phase 2: Static friction measurement ──
    LOG_BOARD("TUNE: phase2 - static friction");
    uint8_t threshold_duties[TUNE_NUM_REPS];

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Ensure piece is centered
      calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);  // no-op if already there
      delay(TUNE_SETTLE_MS);
      uint16_t pre = hw_.readSensor(si);

      uint8_t found_duty = 255;
      for (int d = TUNE_FRICTION_DUTY_STEP; d <= 255; d += TUNE_FRICTION_DUTY_STEP) {
        hw_.pulseBit((uint8_t)coil_bit, TUNE_FRICTION_PULSE_MS, (uint8_t)d);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        if (abs((int)post - (int)pre) > 20) {
          found_duty = (uint8_t)d;
          LOG_BOARD("TUNE: friction rep %d: moved at duty=%d (pre=%d post=%d)", rep, d, pre, post);
          break;
        }

        // Re-center for next duty step
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);  // pull back if it drifted
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        pieces_[TUNE_X + 1][TUNE_Y] = PIECE_NONE;
        delay(TUNE_SETTLE_MS);
        pre = hw_.readSensor(si);  // re-baseline
      }

      threshold_duties[rep] = found_duty;

      // Re-center piece
      // Find where piece is and move back
      for (int x = 0; x < GRID_COLS; x++)
        for (int y = 0; y < GRID_ROWS; y++)
          if (pieces_[x][y] != PIECE_NONE && (x != TUNE_X || y != TUNE_Y)) {
            calMove(x, y, TUNE_X, TUNE_Y);
          }
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    tuneSort(threshold_duties, TUNE_NUM_REPS);
    uint8_t median_duty = threshold_duties[TUNE_NUM_REPS / 2];

    // Convert to friction_static using force model at d=1.0
    float default_force_k = 10.0f;
    float force_at_1 = default_force_k * 1.0f / (powf(1.0f, 2.0f) + eps);
    float fitted_friction_static = force_at_1 * (median_duty / 255.0f);

    LOG_BOARD("TUNE: static friction: median_duty=%d friction_static=%.2f", median_duty, fitted_friction_static);

    // ── Phase 3: Coil force profiling ──
    LOG_BOARD("TUNE: phase3 - force profiling");
    float displacements[TUNE_NUM_DURATIONS];

    for (int di = 0; di < TUNE_NUM_DURATIONS; di++) {
      float disp_sum = 0;
      int valid_reps = 0;

      for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
        // Ensure centered
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
        calMove(TUNE_X, TUNE_Y, TUNE_X, TUNE_Y);
        delay(TUNE_SETTLE_MS);

        uint16_t pre = hw_.readSensor(si);
        hw_.pulseBit((uint8_t)coil_bit, TUNE_DURATIONS[di], 255);
        delay(TUNE_SETTLE_MS);
        uint16_t post = hw_.readSensor(si);

        float strength_pre = baseline - pre;
        float strength_post = baseline - post;

        // Estimate displacement using sensor model
        float d_pre = (strength_pre > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_pre - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 0.0f;
        float d_post = (strength_post > 1.0f) ? powf(fmaxf(fitted_sensor_k / strength_post - eps, 0.001f), 1.0f / fitted_sensor_falloff) : 3.0f;

        float disp = d_post - d_pre;
        if (disp > 0) {
          disp_sum += disp;
          valid_reps++;
        }

        LOG_BOARD("TUNE: force dur=%dms rep=%d pre=%d post=%d disp=%.2f",
                  TUNE_DURATIONS[di], rep, pre, post, disp);

        // Re-center
        calMove(TUNE_X + 1, TUNE_Y, TUNE_X, TUNE_Y);
        calMove(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y);
        memset(pieces_, PIECE_NONE, sizeof(pieces_));
        pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      }

      displacements[di] = (valid_reps > 0) ? disp_sum / valid_reps : 0;
      LOG_BOARD("TUNE: force dur=%dms avg_disp=%.3f", TUNE_DURATIONS[di], displacements[di]);
    }

    // Fit force_k: at d=1, F = force_k * 1/(1+eps).
    // displacement ≈ 0.5 * (F - friction_static) * t^2 for short pulses
    // Use the 100ms data point for fitting
    float t_fit = 0.1f;  // 100ms
    float disp_fit = displacements[3];  // index 3 = 100ms
    float fitted_force_k = default_force_k;
    if (disp_fit > 0.01f) {
      // disp = 0.5 * (F - fric_s) * t^2, F = fk * 1/(1+eps)
      // fk = (2*disp/t^2 + fric_s) * (1+eps)
      fitted_force_k = (2.0f * disp_fit / (t_fit * t_fit) + fitted_friction_static) * (1.0f + eps);
    }

    // Fit friction_kinetic from ratio of displacements
    // Longer pulses show diminishing returns due to friction limiting speed
    float fitted_friction_kinetic = 2.0f;  // default
    if (displacements[4] > 0.01f && displacements[2] > 0.01f) {
      // ratio of 200ms/50ms displacement — if purely kinematic would be 16x, less = more friction
      float ratio = displacements[4] / displacements[2];
      // friction_kinetic ≈ force / terminal_velocity, estimate from saturation
      if (ratio < 10.0f && ratio > 1.0f) {
        float terminal_v = displacements[4] / 0.2f;  // rough v from displacement/time
        float f_net = fitted_force_k * 1.0f / (1.0f + eps);
        fitted_friction_kinetic = f_net / fmaxf(terminal_v, 0.1f);
      }
    }

    LOG_BOARD("TUNE: fitted force_k=%.2f friction_kinetic=%.2f", fitted_force_k, fitted_friction_kinetic);

    // ── Phase 4: Move verification ──
    LOG_BOARD("TUNE: phase4 - verification");
    int success_count = 0;
    uint16_t dest_readings[TUNE_NUM_REPS];
    unsigned long elapsed_ms[TUNE_NUM_REPS];

    // Temporarily set discovered params for verification moves
    PhysicsParams saved_params = physics_params_;
    physics_params_.mu_static = fitted_friction_static;
    physics_params_.mu_kinetic = fitted_friction_kinetic;

    for (int rep = 0; rep < TUNE_NUM_REPS; rep++) {
      // Move piece to (5,3)
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
      calMove(TUNE_X, TUNE_Y, TUNE_X + 2, TUNE_Y);
      pieces_[TUNE_X][TUNE_Y] = PIECE_NONE;
      pieces_[TUNE_X + 2][TUNE_Y] = PIECE_WHITE;
      delay(TUNE_SETTLE_MS);

      // Attempt physics move back
      unsigned long t0 = millis();
      MoveError err = movePhysicsOrthogonal(TUNE_X + 2, TUNE_Y, TUNE_X, TUNE_Y, true);
      elapsed_ms[rep] = millis() - t0;

      delay(100);
      dest_readings[rep] = hw_.readSensor(si);

      bool arrived = (err == MoveError::NONE) && (dest_readings[rep] < baseline - 100);
      if (arrived) success_count++;

      LOG_BOARD("TUNE: verify rep=%d err=%d t=%lums reading=%d %s",
                rep, (int)err, elapsed_ms[rep], dest_readings[rep], arrived ? "OK" : "MISS");

      // Re-center
      memset(pieces_, PIECE_NONE, sizeof(pieces_));
      pieces_[TUNE_X][TUNE_Y] = PIECE_WHITE;
    }

    LOG_BOARD("TUNE: verification: %d/%d success", success_count, TUNE_NUM_REPS);

    // ── Build result JSON ──
    String j = "{\"sensor_fit\":{\"sensor_k\":";
    j += String(fitted_sensor_k, 1);
    j += ",\"sensor_falloff\":";
    j += String(fitted_sensor_falloff, 2);
    j += ",\"readings_d0\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d0[i]); }
    j += "],\"readings_d1\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d1[i]); }
    j += "],\"readings_d2\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(readings_d2[i]); }
    j += "]},\"static_friction\":{\"threshold_duties\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(threshold_duties[i]); }
    j += "],\"median_duty\":";
    j += String(median_duty);
    j += ",\"friction_static\":";
    j += String(fitted_friction_static, 2);
    j += "},\"force_profile\":{\"durations_ms\":[10,25,50,100,200],\"displacements\":[";
    for (int i = 0; i < TUNE_NUM_DURATIONS; i++) { if (i) j += ","; j += String(displacements[i], 3); }
    j += "],\"force_k\":";
    j += String(fitted_force_k, 2);
    j += ",\"friction_kinetic\":";
    j += String(fitted_friction_kinetic, 2);
    j += "},\"verification\":{\"success_count\":";
    j += String(success_count);
    j += ",\"total\":";
    j += String(TUNE_NUM_REPS);
    j += ",\"dest_readings\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(dest_readings[i]); }
    j += "],\"elapsed_ms\":[";
    for (int i = 0; i < TUNE_NUM_REPS; i++) { if (i) j += ","; j += String(elapsed_ms[i]); }
    j += "]},\"recommended\":{";
    j += "\"force_k\":"; j += String(fitted_force_k, 2);
    j += ",\"force_epsilon\":"; j += String(eps, 2);
    j += ",\"falloff_exp\":2.0";
    j += ",\"voltage_scale\":1.0";
    j += ",\"friction_static\":"; j += String(fitted_friction_static, 2);
    j += ",\"friction_kinetic\":"; j += String(fitted_friction_kinetic, 2);
    j += ",\"target_velocity\":5.0";
    j += ",\"target_accel\":20.0";
    j += ",\"max_duration_ms\":5000";
    j += "}}";

    // Restore original params
    physics_params_ = saved_params;

    LOG_BOARD("TUNE: complete");
    initDefaultBoard();
    return j;
  }

  // ── Calibration ─────────────────────────────────────────────

  static constexpr int CAL_BASELINE_SAMPLES = 10000;
  static constexpr int CAL_PIECE_REPEATS = 5;
  static constexpr int CAL_SETTLE_MS = 250;

  CalibrateResponse calibrate() {
    LOG_BOARD("CAL: start");
    memset(&cal_data_, 0, sizeof(cal_data_));
    memset(pieces_, PIECE_NONE, sizeof(pieces_));
    pieces_[0][0] = PIECE_WHITE;

    // Sweep pieces to origin
    if (!calMove(9,6,9,0) || !calMove(6,6,6,0) || !calMove(3,6,3,0) ||
        !calMove(0,6,0,0) || !calMove(9,0,0,0)) return { false };

    // Phase 1: baselines (no piece nearby)
    LOG_BOARD("CAL: baselines - right half (piece at 0,0)");
    calMeasureHalf(2, 3);

    LOG_BOARD("CAL: moving piece (0,0) -> (6,0)");
    if (!calMove(0,0,6,0)) return { false };

    LOG_BOARD("CAL: baselines - left half (piece at 6,0)");
    calMeasureHalf(0, 1);

    // Phase 2: jiggle-measure each sensor with piece
    LOG_BOARD("CAL: phase2 - with piece");
    uint8_t px = 6, py = 0;

    for (int row = 0; row < (int)SR_ROWS; row++) {
      for (int ci = 0; ci < (int)SR_COLS; ci++) {
        int col = (row % 2 == 0) ? ci : ((int)SR_COLS - 1 - ci);
        uint8_t tx = col * SR_BLOCK;
        uint8_t ty = row * SR_BLOCK;

        if (!calMove(px, py, tx, ty)) return { false };
        px = tx; py = ty;

        uint8_t si = col * SR_ROWS + (SR_ROWS - 1 - row);
        auto& cs = cal_data_.sensors[si];
        calJiggleMeasure(tx, ty, si, cs.piece_mean, cs.piece_stddev, cs.piece_median);
        LOG_BOARD("CAL: piece sensor %d (%d,%d): mean=%.1f stddev=%.2f median=%d",
                  si, tx, ty, cs.piece_mean, cs.piece_stddev, cs.piece_median);
      }
    }

    // Phase 3: validation - zig-zag across board
    LOG_BOARD("CAL: phase3 - validation");
    memset(cal_data_.detections, 0, sizeof(cal_data_.detections));

    if (!calMove(px, py, 9, 6)) return { false };
    if (!calValidatePass()) return { false };

    cal_data_.valid = true;
    LOG_BOARD("CAL: complete");
    initDefaultBoard();
    return { true };
  }

  // ── Coil Control ──────────────────────────────────────────

  PulseCoilResponse pulseCoil(uint8_t x, uint8_t y, uint16_t duration_ms) {
    PulseCoilResponse res = { false, PulseError::NONE };

    if (x >= GRID_COLS || y >= GRID_ROWS) { res.error = PulseError::INVALID_COIL; return res; }
    if (duration_ms > MAX_PULSE_MS) { res.error = PulseError::PULSE_TOO_LONG; return res; }
    int8_t bit = coordToBit(x, y);
    if (bit < 0) { res.error = PulseError::INVALID_COIL; return res; }
    if (!hw_.pulseBit((uint8_t)bit, duration_ms)) { res.error = PulseError::THERMAL_LIMIT; return res; }

    res.success = true;
    return res;
  }

  // ── Board State ───────────────────────────────────────────

  GetBoardStateResponse getBoardState() {
    GetBoardStateResponse res = {};

    uint16_t raw[NUM_HALL_SENSORS];
    hw_.readAllSensors(raw, NUM_HALL_SENSORS);
    // Sensors 0-11 map column-major with inverted rows:
    // sensor i → sr_col = i/3, physical row = (2 - i%3)
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      int col = i / SR_ROWS;
      int row = (SR_ROWS - 1) - (i % SR_ROWS);
      res.raw_strengths[col][row] = raw[i];
    }

    memcpy(res.pieces, pieces_, sizeof(pieces_));
    return res;
  }

  // ── RGB / System ──────────────────────────────────────────

  SetRGBResponse setRGB(uint8_t r, uint8_t g, uint8_t b) { hw_.setRGB(r, g, b); return { true }; }
  void shutdown() { hw_.shutdown(); }

  // ── Physics Params (stored, persisted to NVS) ──────────────

  const PhysicsParams& getPhysicsParams() const { return physics_params_; }

  void setPhysicsParams(const PhysicsParams& p) {
    physics_params_ = p;
    savePhysicsParams();
    LOG_BOARD("physics params updated and saved to NVS");
  }

  String physicsParamsToJson() const {
    const auto& p = physics_params_;
    String j = "{";
    j += "\"piece_mass_g\":"; j += String(p.piece_mass_g, 2);
    j += ",\"max_current_a\":"; j += String(p.max_current_a, 2);
    j += ",\"mu_static\":"; j += String(p.mu_static, 3);
    j += ",\"mu_kinetic\":"; j += String(p.mu_kinetic, 3);
    j += ",\"target_velocity_mm_s\":"; j += String(p.target_velocity_mm_s, 1);
    j += ",\"target_accel_mm_s2\":"; j += String(p.target_accel_mm_s2, 1);
    j += ",\"max_jerk_mm_s3\":"; j += String(p.max_jerk_mm_s3, 1);
    j += ",\"coast_friction_offset\":"; j += String(p.coast_friction_offset, 3);
    j += ",\"brake_pulse_ms\":"; j += String(p.brake_pulse_ms);
    j += ",\"pwm_freq_hz\":"; j += String(p.pwm_freq_hz);
    j += ",\"pwm_compensation\":"; j += String(p.pwm_compensation, 2);
    j += ",\"all_coils_equal\":"; j += p.all_coils_equal ? "true" : "false";
    j += ",\"force_scale\":"; j += String(p.force_scale, 2);
    j += ",\"max_duration_ms\":"; j += String(p.max_duration_ms);
    j += ",\"max_retry_attempts\":"; j += String(p.max_retry_attempts);
    j += "}";
    return j;
  }

  // ── Calibration Data Access ─────────────────────────────────

  struct CalSensor {
    float baseline_mean;
    float baseline_stddev;
    float piece_mean;
    float piece_stddev;
    uint16_t piece_median;
  };

  struct CalData {
    bool valid = false;
    CalSensor sensors[NUM_HALL_SENSORS];
    uint8_t detections[SENSOR_COLS][SENSOR_ROWS]; // 0, 1, or 2 from validation passes
  };

  GetCalibrationResponse getCalibration() {
    return { calDataToJson() };
  }

private:
  Hardware hw_;
  uint8_t pieces_[GRID_COLS][GRID_ROWS];
  CalData cal_data_;
  PieceState piece_states_[GRID_COLS][GRID_ROWS];
  PhysicsMove physics_{hw_};
  PhysicsParams physics_params_;
  Preferences prefs_;

  // ── NVS Physics Params ─────────────────────────────────────

  void loadPhysicsParams() {
    prefs_.begin("physics", true);  // read-only
    physics_params_.piece_mass_g       = prefs_.getFloat("mass", physics_params_.piece_mass_g);
    physics_params_.max_current_a      = prefs_.getFloat("current", physics_params_.max_current_a);
    physics_params_.mu_static          = prefs_.getFloat("mu_s", physics_params_.mu_static);
    physics_params_.mu_kinetic         = prefs_.getFloat("mu_k", physics_params_.mu_kinetic);
    physics_params_.target_velocity_mm_s = prefs_.getFloat("vel", physics_params_.target_velocity_mm_s);
    physics_params_.target_accel_mm_s2   = prefs_.getFloat("accel", physics_params_.target_accel_mm_s2);
    physics_params_.max_jerk_mm_s3       = prefs_.getFloat("jerk", physics_params_.max_jerk_mm_s3);
    physics_params_.coast_friction_offset = prefs_.getFloat("coast_f", physics_params_.coast_friction_offset);
    physics_params_.brake_pulse_ms     = prefs_.getUShort("brake", physics_params_.brake_pulse_ms);
    physics_params_.pwm_freq_hz        = prefs_.getUShort("pwm_f", physics_params_.pwm_freq_hz);
    physics_params_.pwm_compensation   = prefs_.getFloat("pwm_c", physics_params_.pwm_compensation);
    physics_params_.all_coils_equal    = prefs_.getBool("eq_coil", physics_params_.all_coils_equal);
    physics_params_.force_scale        = prefs_.getFloat("f_scale", physics_params_.force_scale);
    physics_params_.max_duration_ms    = prefs_.getUShort("timeout", physics_params_.max_duration_ms);
    physics_params_.max_retry_attempts = prefs_.getUChar("retries", physics_params_.max_retry_attempts);
    prefs_.end();
    LOG_BOARD("physics params loaded from NVS (I=%.2fA v=%.0f a=%.0f)",
              physics_params_.max_current_a, physics_params_.target_velocity_mm_s, physics_params_.target_accel_mm_s2);
  }

  void savePhysicsParams() {
    prefs_.begin("physics", false);  // read-write
    prefs_.putFloat("mass", physics_params_.piece_mass_g);
    prefs_.putFloat("current", physics_params_.max_current_a);
    prefs_.putFloat("mu_s", physics_params_.mu_static);
    prefs_.putFloat("mu_k", physics_params_.mu_kinetic);
    prefs_.putFloat("vel", physics_params_.target_velocity_mm_s);
    prefs_.putFloat("accel", physics_params_.target_accel_mm_s2);
    prefs_.putFloat("jerk", physics_params_.max_jerk_mm_s3);
    prefs_.putFloat("coast_f", physics_params_.coast_friction_offset);
    prefs_.putUShort("brake", physics_params_.brake_pulse_ms);
    prefs_.putUShort("pwm_f", physics_params_.pwm_freq_hz);
    prefs_.putFloat("pwm_c", physics_params_.pwm_compensation);
    prefs_.putBool("eq_coil", physics_params_.all_coils_equal);
    prefs_.putFloat("f_scale", physics_params_.force_scale);
    prefs_.putUShort("timeout", physics_params_.max_duration_ms);
    prefs_.putUChar("retries", physics_params_.max_retry_attempts);
    prefs_.end();
  }

  // ── Default Board ───────────────────────────────────────────
  // 3 white on bottom row, 3 black on top row
  // Missing 4th piece on each side (only 3 per side)

  void initDefaultBoard() {
    memset(pieces_, PIECE_NONE, sizeof(pieces_));

    // 3 white at y=0
    pieces_[0][0] = PIECE_WHITE;
    pieces_[3][0] = PIECE_WHITE;
    pieces_[6][0] = PIECE_WHITE;

    // 3 black at y=6
    pieces_[0][6] = PIECE_BLACK;
    pieces_[3][6] = PIECE_BLACK;
    pieces_[6][6] = PIECE_BLACK;

    // Reset physics states
    for (int x = 0; x < GRID_COLS; x++)
      for (int y = 0; y < GRID_ROWS; y++)
        piece_states_[x][y].reset(x * GRID_TO_MM, y * GRID_TO_MM);

    LOG_BOARD("initDefaultBoard: 3 white at y=0, 3 black at y=6");
  }

  // ── Calibration Helpers ──────────────────────────────────────

  // Baseline: 10k samples, mean + stddev
  void calMeasureBaseline(uint8_t si, float& out_mean, float& out_stddev) {
    double sum = 0, sum_sq = 0;
    for (int s = 0; s < CAL_BASELINE_SAMPLES; s++) {
      uint16_t val = hw_.readSensor(si);
      sum += val;
      sum_sq += (double)val * val;
    }
    out_mean = (float)(sum / CAL_BASELINE_SAMPLES);
    out_stddev = (float)sqrt((sum_sq / CAL_BASELINE_SAMPLES) - (double)out_mean * out_mean);
  }

  // Measure baselines for sensors in the given sr_col range
  void calMeasureHalf(int colMin, int colMax) {
    for (int si = 0; si < NUM_HALL_SENSORS; si++) {
      int col = si / SR_ROWS;
      if (col < colMin || col > colMax) continue;
      int row = (SR_ROWS - 1) - (si % SR_ROWS);

      calMeasureBaseline(si, cal_data_.sensors[si].baseline_mean, cal_data_.sensors[si].baseline_stddev);
      LOG_BOARD("CAL: baseline sensor %d (%d,%d): mean=%.1f stddev=%.2f",
                si, col * SR_BLOCK, row * SR_BLOCK,
                cal_data_.sensors[si].baseline_mean, cal_data_.sensors[si].baseline_stddev);
    }
  }

  // Jiggle-measure: move to position, read, jiggle off and back, read again.
  // Repeat CAL_PIECE_REPEATS times, compute mean + stddev from single readings.
  void calJiggleMeasure(uint8_t x, uint8_t y, uint8_t si,
                        float& out_mean, float& out_stddev, uint16_t& out_median) {
    uint8_t sr_row = y / SR_BLOCK;
    int8_t jdy = (sr_row == SR_ROWS - 1) ? -1 : 1;
    uint8_t jy = (uint8_t)(y + jdy);

    uint16_t vals[CAL_PIECE_REPEATS];
    double sum = 0, sum_sq = 0;

    for (int i = 0; i < CAL_PIECE_REPEATS; i++) {
      calMove(x, y, x, jy);
      calMove(x, jy, x, y);
      delay(CAL_SETTLE_MS);

      vals[i] = hw_.readSensor(si);
      sum += vals[i];
      sum_sq += (double)vals[i] * vals[i];
      LOG_BOARD("CAL: jiggle %d/%d sensor %d val=%d", i + 1, CAL_PIECE_REPEATS, si, vals[i]);
    }

    // Sort for median (insertion sort, tiny array)
    for (int i = 1; i < CAL_PIECE_REPEATS; i++) {
      uint16_t key = vals[i];
      int j = i - 1;
      while (j >= 0 && vals[j] > key) { vals[j + 1] = vals[j]; j--; }
      vals[j + 1] = key;
    }

    out_mean = (float)(sum / CAL_PIECE_REPEATS);
    out_stddev = (float)sqrt((sum_sq / CAL_PIECE_REPEATS) - (double)out_mean * out_mean);
    out_median = vals[CAL_PIECE_REPEATS / 2];
  }

  // Check sensor at position, set detection to 1 if below threshold
  void calCheckSensor(uint8_t x, uint8_t y) {
    uint8_t col = x / SR_BLOCK;
    uint8_t row = y / SR_BLOCK;
    uint8_t si = col * SR_ROWS + (SR_ROWS - 1 - row);
    auto& s = cal_data_.sensors[si];

    /* delay(CAL_SETTLE_MS); */
    uint16_t val = hw_.readSensor(si);
    float threshold = (s.baseline_mean + s.piece_mean) / 2.0f;
    bool detected = (val < threshold);

    if (detected) cal_data_.detections[col][row] = 1;
    LOG_BOARD("CAL: validate (%d,%d) sensor=%d val=%d thresh=%.0f %s",
              x, y, si, val, threshold, detected ? "DETECTED" : "miss");
  }

  // Validation: horizontal zig-zag from top-right to bottom-left
  bool calValidatePass() {
    uint8_t px = 9, py = 6;

    for (int row = (int)SR_ROWS - 1; row >= 0; row--) {
      for (int ci = 0; ci < (int)SR_COLS; ci++) {
        int col = (row % 2 == 0) ? ((int)SR_COLS - 1 - ci) : ci;
        uint8_t tx = col * SR_BLOCK;
        uint8_t ty = row * SR_BLOCK;
        if (tx != px || ty != py) {
          if (!calMove(px, py, tx, ty)) return false;
          px = tx; py = ty;
        }
        calCheckSensor(px, py);
      }
    }
    return true;
  }

  // Map grid position to sensor index (column-major, inverted rows)
  static uint8_t sensorForGrid(uint8_t x, uint8_t y) {
    return (x / SR_BLOCK) * SR_ROWS + (SR_ROWS - 1 - (y / SR_BLOCK));
  }

  // Insertion sort for small arrays (used by tuning)
  static void tuneSort(uint16_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint16_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }

  static void tuneSort(uint8_t* arr, int n) {
    for (int i = 1; i < n; i++) {
      uint8_t key = arr[i];
      int j = i - 1;
      while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
      arr[j + 1] = key;
    }
  }

  // Wrapper: move with skipValidation, handles diagonals, log on error
  bool calMove(uint8_t fx, uint8_t fy, uint8_t tx, uint8_t ty) {
    if (fx == tx && fy == ty) return true;

    // Diagonal: move x first, then y
    if (fx != tx && fy != ty) {
      if (!calMove(fx, fy, tx, fy)) return false;
      return calMove(tx, fy, tx, ty);
    }

    MoveError err = moveDumbOrthogonal(fx, fy, tx, ty, true);
    if (err != MoveError::NONE) {
      LOG_BOARD("CAL: move (%d,%d)->(%d,%d) failed: %d", fx, fy, tx, ty, (int)err);
    }
    return err == MoveError::NONE;
  }



  // ── Calibration JSON Serialization ──────────────────────────

  String calDataToJson() {
    if (!cal_data_.valid) return Json().add("valid", false).build();

    String j = "{\"valid\":true,\"sensors\":[";
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      if (i) j += ",";
      auto& s = cal_data_.sensors[i];
      j += "{\"baseline_mean\":";
      j += String(s.baseline_mean, 1);
      j += ",\"baseline_stddev\":";
      j += String(s.baseline_stddev, 2);
      j += ",\"piece_mean\":";
      j += String(s.piece_mean, 1);
      j += ",\"piece_stddev\":";
      j += String(s.piece_stddev, 2);
      j += ",\"piece_median\":";
      j += String(s.piece_median);
      j += "}";
    }
    j += "],\"detections\":[";
    for (int col = 0; col < SENSOR_COLS; col++) {
      if (col) j += ",";
      j += "[";
      for (int row = 0; row < SENSOR_ROWS; row++) {
        if (row) j += ",";
        j += String(cal_data_.detections[col][row]);
      }
      j += "]";
    }
    j += "]}";
    return j;
  }

  // ── Coil Grid Mapping ─────────────────────────────────────

  static constexpr uint8_t SR_COLS = 4;
  static constexpr uint8_t SR_ROWS = 3;
  static constexpr uint8_t SR_BLOCK = 3;

  static int8_t coordToBit(uint8_t x, uint8_t y) {
    uint8_t sr_col = x / SR_BLOCK;
    uint8_t sr_row = y / SR_BLOCK;
    if (sr_col >= SR_COLS || sr_row >= SR_ROWS) return -1;

    uint8_t sr_index = sr_col * SR_ROWS + sr_row;
    uint8_t lx = x % SR_BLOCK;
    uint8_t ly = y % SR_BLOCK;

    int8_t local_bit = -1;
    if (ly == 0) local_bit = 2 - lx;
    else if (lx == 0) local_bit = 2 + ly;
    if (local_bit < 0) return -1;

    return (int8_t)(sr_index * 8 + local_bit);
  }
};
