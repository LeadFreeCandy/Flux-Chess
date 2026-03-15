#pragma once
#include <Arduino.h>
#include "board.h"

class SerialServer {
public:
  SerialServer(Board& board) : board_(board) {}

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
  Board& board_;
  String line_buf_;

  // ── JSON Helpers ────────────────────────────────────────────

  static String jsonGet(const String& json, const char* key) {
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

  static String jsonGetObj(const String& json, const char* key) {
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

  static void sendResponse(const char* method, const String& resultJson) {
    Serial.printf("{\"method\":\"%s\",\"result\":%s}\n", method, resultJson.c_str());
  }

  static void sendError(const char* method, const char* error) {
    Serial.printf("{\"method\":\"%s\",\"error\":\"%s\"}\n", method, error);
  }

  // ── Command Dispatch ────────────────────────────────────────

  void handleCommand(const String& line) {
    String method = jsonGet(line, "method");
    if (method.length() == 0) return;

    String params = jsonGetObj(line, "params");

    if (method == "pulse_coil") {
      int x = jsonGet(params, "x").toInt();
      int y = jsonGet(params, "y").toInt();
      int dur = jsonGet(params, "duration_ms").toInt();
      auto res = board_.pulseCoil(x, y, dur);
      String err = pulseErrorStr(res.error);
      sendResponse("pulse_coil",
        String("{\"success\":") + (res.success ? "true" : "false") +
        ",\"error\":\"" + err + "\"}");

    } else if (method == "get_board_state") {
      auto res = board_.getBoardState();
      String json = "{\"raw_strengths\":[";
      for (int x = 0; x < GRID_COLS; x++) {
        json += "[";
        for (int y = 0; y < GRID_ROWS; y++) {
          json += String(res.raw_strengths[x][y]);
          if (y < GRID_ROWS - 1) json += ",";
        }
        json += "]";
        if (x < GRID_COLS - 1) json += ",";
      }
      json += "],\"piece_count\":" + String(res.piece_count) + "}";
      sendResponse("get_board_state", json);

    } else if (method == "set_rgb") {
      int r = jsonGet(params, "r").toInt();
      int g = jsonGet(params, "g").toInt();
      int b = jsonGet(params, "b").toInt();
      auto res = board_.setRGB(r, g, b);
      sendResponse("set_rgb",
        String("{\"success\":") + (res.success ? "true" : "false") + "}");

    } else if (method == "shutdown") {
      sendResponse("shutdown", "{}");
      delay(50);
      board_.shutdown();

    } else {
      sendError(method.c_str(), "unknown command");
    }
  }

  static String pulseErrorStr(PulseError e) {
    switch (e) {
      case PulseError::NONE: return "NONE";
      case PulseError::INVALID_COIL: return "INVALID_COIL";
      case PulseError::PULSE_TOO_LONG: return "PULSE_TOO_LONG";
      case PulseError::THERMAL_LIMIT: return "THERMAL_LIMIT";
    }
    return "NONE";
  }
};
