// FluxChess ESP32 Firmware — Serial JSON API
// No libraries, no partitions, just Serial.printf

#include "api.h"

String lineBuf;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("{\"type\":\"ready\"}");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf.trim();
      if (lineBuf.length() > 0) {
        handleCommand(lineBuf);
      }
      lineBuf = "";
    } else {
      lineBuf += c;
    }
  }
}

// ── Minimal JSON parser (no ArduinoJson) ──────────────────────

String jsonGet(const String& json, const char* key) {
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

String jsonGetObj(const String& json, const char* key) {
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

// ── Command handler ───────────────────────────────────────────

void handleCommand(const String& line) {
  String method = jsonGet(line, "method");
  if (method.length() == 0) return;

  String params = jsonGetObj(line, "params");

  if (method == "pulse_coil") {
    int x = jsonGet(params, "x").toInt();
    int y = jsonGet(params, "y").toInt();
    int dur = jsonGet(params, "duration_ms").toInt();

    if (x >= GRID_COLS || y >= GRID_ROWS) {
      Serial.printf("{\"method\":\"pulse_coil\",\"result\":{\"success\":false,\"error\":\"INVALID_COIL\"}}\n");
    } else if (dur > MAX_PULSE_MS) {
      Serial.printf("{\"method\":\"pulse_coil\",\"result\":{\"success\":false,\"error\":\"PULSE_TOO_LONG\"}}\n");
    } else {
      // TODO: actually pulse the coil
      Serial.printf("{\"method\":\"pulse_coil\",\"result\":{\"success\":true,\"error\":\"NONE\"}}\n");
    }

  } else if (method == "get_board_state") {
    Serial.print("{\"method\":\"get_board_state\",\"result\":{\"raw_strengths\":[");
    for (int x = 0; x < GRID_COLS; x++) {
      Serial.print("[");
      for (int y = 0; y < GRID_ROWS; y++) {
        Serial.print(0);  // stub — no ADC reads yet
        if (y < GRID_ROWS - 1) Serial.print(",");
      }
      Serial.print("]");
      if (x < GRID_COLS - 1) Serial.print(",");
    }
    Serial.printf("],\"piece_count\":0}}\n");

  } else if (method == "set_rgb") {
    // TODO: drive RGB LEDs
    Serial.printf("{\"method\":\"set_rgb\",\"result\":{\"success\":true}}\n");

  } else if (method == "shutdown") {
    Serial.printf("{\"method\":\"shutdown\",\"result\":{}}\n");
    delay(100);
    ESP.restart();

  } else {
    Serial.printf("{\"method\":\"%s\",\"error\":\"unknown command\"}\n", method.c_str());
  }
}
