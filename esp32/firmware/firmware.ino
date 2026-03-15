#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "pins.h"
#include "transport.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize ADC for hall sensors
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < NUM_HALL_SENSORS; i++) {
    pinMode(HALL_PINS[i], INPUT);
  }

  // Initialize shift register pins
  pinMode(PIN_SR_DATA, OUTPUT);
  pinMode(PIN_SR_CLOCK, OUTPUT);
  pinMode(PIN_SR_LATCH, OUTPUT);
  pinMode(PIN_SR_OE, OUTPUT);
  digitalWrite(PIN_SR_OE, HIGH); // Outputs disabled

  // Mount LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("{\"type\":\"error\",\"msg\":\"LittleFS mount failed\"}");
  }

  // Connect to WiFi (non-blocking, timeout after 10s)
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("{\"type\":\"wifi\",\"ip\":\"%s\"}\n",
                    WiFi.localIP().toString().c_str());
    } else {
      Serial.println("{\"type\":\"wifi\",\"ip\":null}");
    }
  }

  // Setup HTTP routes and start server
  setup_http_routes(server);
  server.begin();

  Serial.println("{\"type\":\"ready\"}");
}

void loop() {
  handle_serial_command();
}
