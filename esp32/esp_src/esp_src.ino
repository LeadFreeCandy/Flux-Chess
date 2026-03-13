const uint8_t adcPins[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

unsigned long lastSendMs = 0;
const uint32_t sendIntervalMs = 50; // 20 Hz

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  for (int i = 0; i < 9; i++) {
    pinMode(adcPins[i], INPUT);
  }

  Serial.println("{\"type\":\"hello\",\"msg\":\"serial adc streamer ready\"}");
}

void loop() {
  unsigned long now = millis();
  if (now - lastSendMs >= sendIntervalMs) {
    lastSendMs = now;
    sendAdcFrame();
  }
}

void sendAdcFrame() {
  int values[9];
  for (int i = 0; i < 9; i++) {
    values[i] = analogRead(adcPins[i]);
  }

  Serial.printf(
    "{\"type\":\"adc\",\"values\":[%d,%d,%d,%d,%d,%d,%d,%d,%d]}\n",
    values[0], values[1], values[2],
    values[3], values[4], values[5],
    values[6], values[7], values[8]
  );
}
