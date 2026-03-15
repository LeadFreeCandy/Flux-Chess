#pragma once
#include <Arduino.h>
#include <functional>
#include "board.h"

// ── JSON Parse Helpers (reusable by future web server) ────────

inline String jsonGet(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int idx = json.indexOf(search);
  if (idx < 0) return "";
  idx = json.indexOf(':', idx);
  if (idx < 0) return "";
  idx++;
  while (idx < (int)json.length() && json[idx] == ' ') idx++;
  if (json[idx] == '"') {
    int end = json.indexOf('"', idx + 1);
    return json.substring(idx + 1, end);
  }
  int end = idx;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}') end++;
  return json.substring(idx, end);
}

inline String jsonGetObj(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int idx = json.indexOf(search);
  if (idx < 0) return "";
  idx = json.indexOf('{', idx);
  if (idx < 0) return "";
  int depth = 0;
  for (int i = idx; i < (int)json.length(); i++) {
    if (json[i] == '{') depth++;
    if (json[i] == '}') depth--;
    if (depth == 0) return json.substring(idx, i + 1);
  }
  return "";
}

// ── Command Registry ──────────────────────────────────────────

using CommandHandler = std::function<String(Board& board, const String& params)>;

struct Command {
  const char* name;
  CommandHandler handler;
};

// ── Built-in command handlers ─────────────────────────────────

inline String handlePulseCoil(Board& board, const String& params) {
  uint8_t x = jsonGet(params, "x").toInt();
  uint8_t y = jsonGet(params, "y").toInt();
  uint16_t dur = jsonGet(params, "duration_ms").toInt();
  return board.pulseCoil(x, y, dur).toJson();
}

inline String handleGetBoardState(Board& board, const String& params) {
  (void)params;
  return board.getBoardState().toJson();
}

inline String handleSetRGB(Board& board, const String& params) {
  uint8_t r = jsonGet(params, "r").toInt();
  uint8_t g = jsonGet(params, "g").toInt();
  uint8_t b = jsonGet(params, "b").toInt();
  return board.setRGB(r, g, b).toJson();
}

inline String handleShutdown(Board& board, const String& params) {
  (void)params;
  return ShutdownResponse{}.toJson();
}

// ── Serial Server ─────────────────────────────────────────────

class SerialServer {
public:
  SerialServer(Board& board) : board_(board), num_commands_(0) {
    on("pulse_coil", handlePulseCoil);
    on("get_board_state", handleGetBoardState);
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
