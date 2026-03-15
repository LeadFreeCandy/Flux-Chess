#include "api.h"
#include <ArduinoJson.h>

PulseCoilResponse handle_pulse_coil(const PulseCoilRequest& req) {
  PulseCoilResponse res = { false, PulseError::NONE };
  if (req.x >= GRID_COLS || req.y >= GRID_ROWS) {
    res.error = PulseError::INVALID_COIL;
    return res;
  }
  if (req.duration_ms > MAX_PULSE_MS) {
    res.error = PulseError::PULSE_TOO_LONG;
    return res;
  }
  res.success = true;
  return res;
}

GetBoardStateResponse handle_get_board_state() {
  GetBoardStateResponse res = {};
  res.piece_count = 0;
  return res;
}

SetRGBResponse handle_set_rgb(const SetRGBRequest& req) {
  (void)req;
  return { true };
}

static const char* pulse_error_str(PulseError e) {
  switch (e) {
    case PulseError::NONE: return "NONE";
    case PulseError::INVALID_COIL: return "INVALID_COIL";
    case PulseError::PULSE_TOO_LONG: return "PULSE_TOO_LONG";
    case PulseError::THERMAL_LIMIT: return "THERMAL_LIMIT";
  }
  return "NONE";
}

void setup() {
  Serial.begin(115200);
  delay(1000);
}

void loop() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  JsonDocument req_doc;
  if (deserializeJson(req_doc, line)) return;

  const char* method = req_doc["method"];
  if (!method) return;

  JsonDocument res_doc;
  res_doc["method"] = method;

  if (strcmp(method, "pulse_coil") == 0) {
    PulseCoilRequest req = {};
    req.x = req_doc["params"]["x"] | 0;
    req.y = req_doc["params"]["y"] | 0;
    req.duration_ms = req_doc["params"]["duration_ms"] | 0;
    auto res = handle_pulse_coil(req);
    res_doc["result"]["success"] = res.success;
    res_doc["result"]["error"] = pulse_error_str(res.error);
  } else if (strcmp(method, "get_board_state") == 0) {
    auto res = handle_get_board_state();
    JsonArray strengths = res_doc["result"]["raw_strengths"].to<JsonArray>();
    for (int x = 0; x < GRID_COLS; x++) {
      JsonArray row = strengths.add<JsonArray>();
      for (int y = 0; y < GRID_ROWS; y++) {
        row.add(res.raw_strengths[x][y]);
      }
    }
    res_doc["result"]["piece_count"] = res.piece_count;
  } else if (strcmp(method, "set_rgb") == 0) {
    SetRGBRequest req = {};
    req.r = req_doc["params"]["r"] | 0;
    req.g = req_doc["params"]["g"] | 0;
    req.b = req_doc["params"]["b"] | 0;
    auto res = handle_set_rgb(req);
    res_doc["result"]["success"] = res.success;
  } else if (strcmp(method, "shutdown") == 0) {
    res_doc["result"].to<JsonObject>();
    String out;
    serializeJson(res_doc, out);
    Serial.println(out);
    delay(100);
    ESP.restart();
    return;
  } else {
    res_doc["error"] = "unknown command";
  }

  String out;
  serializeJson(res_doc, out);
  Serial.println(out);
}
