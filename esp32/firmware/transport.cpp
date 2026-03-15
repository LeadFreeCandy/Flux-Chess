#include "transport.h"
#include "api.h"
#include "api_handlers.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <LittleFS.h>

// ── JSON Serialization Helpers ────────────────────────────────

static const char* pulse_error_str(PulseError e) {
  switch (e) {
    case PulseError::NONE: return "NONE";
    case PulseError::INVALID_COIL: return "INVALID_COIL";
    case PulseError::PULSE_TOO_LONG: return "PULSE_TOO_LONG";
    case PulseError::THERMAL_LIMIT: return "THERMAL_LIMIT";
  }
  return "NONE";
}

// ── HTTP Routes ───────────────────────────────────────────────

static void add_cors_headers(AsyncWebServerResponse* response) {
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setup_http_routes(AsyncWebServer& server) {
  // CORS preflight
  server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
    auto* response = request->beginResponse(204);
    add_cors_headers(response);
    request->send(response);
  });

  // POST /api/shutdown
  server.on("/api/shutdown", HTTP_POST, [](AsyncWebServerRequest* request) {
    ShutdownRequest req = {};
    handle_shutdown(req);
    auto* response = request->beginResponse(200, "application/json", "{}");
    add_cors_headers(response);
    request->send(response);
  });

  // POST /api/pulse_coil
  server.addHandler(new AsyncCallbackJsonWebHandler(
    "/api/pulse_coil",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      PulseCoilRequest req = {};
      req.x = json["x"] | 0;
      req.y = json["y"] | 0;
      req.duration_ms = json["duration_ms"] | 0;

      auto res = handle_pulse_coil(req);

      JsonDocument doc;
      doc["success"] = res.success;
      doc["error"] = pulse_error_str(res.error);

      String body;
      serializeJson(doc, body);
      auto* response = request->beginResponse(200, "application/json", body);
      add_cors_headers(response);
      request->send(response);
    }
  ));

  // GET /api/board_state
  server.on("/api/board_state", HTTP_GET, [](AsyncWebServerRequest* request) {
    GetBoardStateRequest req = {};
    auto res = handle_get_board_state(req);

    JsonDocument doc;
    JsonArray strengths = doc["raw_strengths"].to<JsonArray>();
    for (int x = 0; x < GRID_COLS; x++) {
      JsonArray row = strengths.add<JsonArray>();
      for (int y = 0; y < GRID_ROWS; y++) {
        row.add(res.raw_strengths[x][y]);
      }
    }

    JsonArray pieces = doc["pieces"].to<JsonArray>();
    for (int i = 0; i < res.piece_count; i++) {
      JsonObject p = pieces.add<JsonObject>();
      p["piece_id"] = res.pieces[i].piece_id;
      JsonObject pos = p["pos"].to<JsonObject>();
      pos["x"] = res.pieces[i].pos.x;
      pos["y"] = res.pieces[i].pos.y;
    }
    doc["piece_count"] = res.piece_count;

    String body;
    serializeJson(doc, body);
    auto* response = request->beginResponse(200, "application/json", body);
    add_cors_headers(response);
    request->send(response);
  });

  // POST /api/set_rgb
  server.addHandler(new AsyncCallbackJsonWebHandler(
    "/api/set_rgb",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      SetRGBRequest req = {};
      req.r = json["r"] | 0;
      req.g = json["g"] | 0;
      req.b = json["b"] | 0;

      auto res = handle_set_rgb(req);

      JsonDocument doc;
      doc["success"] = res.success;

      String body;
      serializeJson(doc, body);
      auto* response = request->beginResponse(200, "application/json", body);
      add_cors_headers(response);
      request->send(response);
    }
  ));

  // Serve frontend from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

// ── Serial Dispatch ───────────────────────────────────────────

void handle_serial_command() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  JsonDocument req_doc;
  DeserializationError err = deserializeJson(req_doc, line);
  if (err) return;

  const char* method = req_doc["method"];
  if (!method) return;

  JsonDocument res_doc;
  res_doc["method"] = method;

  if (strcmp(method, "shutdown") == 0) {
    handle_shutdown({});
    res_doc["result"].to<JsonObject>();

  } else if (strcmp(method, "pulse_coil") == 0) {
    PulseCoilRequest req = {};
    req.x = req_doc["params"]["x"] | 0;
    req.y = req_doc["params"]["y"] | 0;
    req.duration_ms = req_doc["params"]["duration_ms"] | 0;
    auto res = handle_pulse_coil(req);
    res_doc["result"]["success"] = res.success;
    res_doc["result"]["error"] = pulse_error_str(res.error);

  } else if (strcmp(method, "get_board_state") == 0) {
    auto res = handle_get_board_state({});
    JsonArray strengths = res_doc["result"]["raw_strengths"].to<JsonArray>();
    for (int x = 0; x < GRID_COLS; x++) {
      JsonArray row = strengths.add<JsonArray>();
      for (int y = 0; y < GRID_ROWS; y++) {
        row.add(res.raw_strengths[x][y]);
      }
    }
    JsonArray pieces = res_doc["result"]["pieces"].to<JsonArray>();
    for (int i = 0; i < res.piece_count; i++) {
      JsonObject p = pieces.add<JsonObject>();
      p["piece_id"] = res.pieces[i].piece_id;
      JsonObject pos = p["pos"].to<JsonObject>();
      pos["x"] = res.pieces[i].pos.x;
      pos["y"] = res.pieces[i].pos.y;
    }
    res_doc["result"]["piece_count"] = res.piece_count;

  } else if (strcmp(method, "set_rgb") == 0) {
    SetRGBRequest req = {};
    req.r = req_doc["params"]["r"] | 0;
    req.g = req_doc["params"]["g"] | 0;
    req.b = req_doc["params"]["b"] | 0;
    auto res = handle_set_rgb(req);
    res_doc["result"]["success"] = res.success;

  } else {
    res_doc["error"] = "unknown command";
  }

  String out;
  serializeJson(res_doc, out);
  Serial.println(out);
}
