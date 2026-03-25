#pragma once
#include <functional>
#include "board.h"

// ── Command Registry ──────────────────────────────────────────

using CommandHandler = std::function<String(Board& board, const String& params)>;

struct Command {
  const char* name;
  CommandHandler handler;
};

// ── Built-in command handlers ─────────────────────────────────

inline String handlePulseCoil(Board& board, const String& params) {
  return board.pulseCoil(
    jsonGet(params, "x").toInt(),
    jsonGet(params, "y").toInt(),
    jsonGet(params, "duration_ms").toInt()
  ).toJson();
}

inline String handleGetBoardState(Board& board, const String&) {
  return board.getBoardState().toJson();
}

inline String handleSetRGB(Board& board, const String& params) {
  return board.setRGB(
    jsonGet(params, "r").toInt(),
    jsonGet(params, "g").toInt(),
    jsonGet(params, "b").toInt()
  ).toJson();
}

inline String handleSetPiece(Board& board, const String& params) {
  uint8_t x = jsonGet(params, "x").toInt();
  uint8_t y = jsonGet(params, "y").toInt();
  uint8_t id = jsonGet(params, "id").toInt();
  board.setPiece(x, y, id);
  return Json().add("success", true).build();
}

inline String handleMoveDumb(Board& board, const String& params) {
  uint8_t fromX = jsonGet(params, "from_x").toInt();
  uint8_t fromY = jsonGet(params, "from_y").toInt();
  uint8_t toX = jsonGet(params, "to_x").toInt();
  uint8_t toY = jsonGet(params, "to_y").toInt();
  MoveError err = board.moveDumb(fromX, fromY, toX, toY);
  return Json()
    .add("success", err == MoveError::NONE)
    .add("error", toJson(err))
    .build();
}

inline String handleShutdown(Board& board, const String&) {
  return ShutdownResponse{}.toJson();
}

// ── Serial Server ─────────────────────────────────────────────

class SerialServer {
public:
  SerialServer(Board& board) : board_(board), num_commands_(0) {
    on("pulse_coil", handlePulseCoil);
    on("get_board_state", handleGetBoardState);
    on("set_piece", handleSetPiece);
    on("move_dumb", handleMoveDumb);
    on("set_rgb", handleSetRGB);
    on("shutdown", handleShutdown);
  }

  void on(const char* name, CommandHandler handler) {
    if (num_commands_ < MAX_COMMANDS) {
      commands_[num_commands_++] = { name, handler };
    }
  }

  void poll() {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        line_buf_.trim();
        if (line_buf_.length() > 0) {
          handleCommand(line_buf_);
        }
        line_buf_ = "";
      } else {
        line_buf_ += c;
      }
    }
  }

private:
  static const int MAX_COMMANDS = 32;
  Board& board_;
  String line_buf_;
  Command commands_[MAX_COMMANDS];
  int num_commands_;

  void handleCommand(const String& line) {
    String method = jsonGet(line, "method");
    if (method.length() == 0) return;

    String params = jsonGetObj(line, "params");

    for (int i = 0; i < num_commands_; i++) {
      if (method == commands_[i].name) {
        String result = commands_[i].handler(board_, params);
        Serial.printf("{\"method\":\"%s\",\"result\":%s}\n",
                      commands_[i].name, result.c_str());
        if (method == "shutdown") {
          delay(50);
          board_.shutdown();
        }
        return;
      }
    }

    Serial.printf("{\"method\":\"%s\",\"error\":\"unknown command\"}\n", method.c_str());
  }
};
