#pragma once
#include <Arduino.h>
#include "pins.h"

// Minimum milliseconds between pulses on the same coil
#define THERMAL_COOLDOWN_MS 500

class Hardware {
public:
  Hardware() {
    // Shift register pins
    pinMode(PIN_SR_DATA, OUTPUT);
    pinMode(PIN_SR_CLOCK, OUTPUT);
    pinMode(PIN_SR_LATCH, OUTPUT);
    pinMode(PIN_SR_OE, OUTPUT);
    digitalWrite(PIN_SR_OE, HIGH);  // OE active low — start disabled

    // Hall sensor ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      pinMode(HALL_PINS[i], INPUT);
    }

    // DC connector sense
    pinMode(PIN_DC1, INPUT);
    pinMode(PIN_DC2, INPUT);

    // Initialize state
    memset(sr_state_, 0, sizeof(sr_state_));
    memset(last_pulse_ms_, 0, sizeof(last_pulse_ms_));

    // Blank shift registers
    srClear();
  }

  // ── Shift Registers ───────────────────────────────────────

  void srWrite() {
    // Shift out all bytes, MSB first, last SR first (chain order)
    for (int i = NUM_SHIFT_REGISTERS - 1; i >= 0; i--) {
      for (int bit = 7; bit >= 0; bit--) {
        digitalWrite(PIN_SR_DATA, (sr_state_[i] >> bit) & 1);
        digitalWrite(PIN_SR_CLOCK, HIGH);
        digitalWrite(PIN_SR_CLOCK, LOW);
      }
    }
    // Latch
    digitalWrite(PIN_SR_LATCH, HIGH);
    digitalWrite(PIN_SR_LATCH, LOW);
  }

  void srSetBit(uint8_t bit, bool val) {
    if (bit >= SR_CHAIN_BITS) return;
    uint8_t reg = bit / 8;
    uint8_t pos = bit % 8;
    if (val) {
      sr_state_[reg] |= (1 << pos);
    } else {
      sr_state_[reg] &= ~(1 << pos);
    }
  }

  void srClear() {
    memset(sr_state_, 0, sizeof(sr_state_));
    srWrite();
  }

  void srSetOE(bool enabled) {
    digitalWrite(PIN_SR_OE, enabled ? LOW : HIGH);  // Active low
  }

  // ── Hall Sensors ──────────────────────────────────────────

  uint16_t readSensor(uint8_t index) {
    if (index >= NUM_HALL_SENSORS) return 0;
    return analogRead(HALL_PINS[index]);
  }

  void readAllSensors(uint16_t* out, uint8_t count) {
    for (uint8_t i = 0; i < count && i < NUM_HALL_SENSORS; i++) {
      out[i] = analogRead(HALL_PINS[i]);
    }
  }

  // ── Capacitive Touch Buttons ──────────────────────────────

  uint16_t readButton1() { return touchRead(PIN_BTN1); }
  uint16_t readButton2() { return touchRead(PIN_BTN2); }

  // ── DC Connector Sense ────────────────────────────────────

  bool readDC1() { return digitalRead(PIN_DC1); }
  bool readDC2() { return digitalRead(PIN_DC2); }

  // ── Thermal Protection ────────────────────────────────────

  bool canPulse(uint8_t coilBit) {
    if (coilBit >= SR_CHAIN_BITS) return false;
    return (millis() - last_pulse_ms_[coilBit]) >= THERMAL_COOLDOWN_MS;
  }

  void recordPulse(uint8_t coilBit) {
    if (coilBit >= SR_CHAIN_BITS) return;
    last_pulse_ms_[coilBit] = millis();
  }

  // ── Shutdown ──────────────────────────────────────────────

  void shutdown() {
    srClear();
    srSetOE(false);
    delay(50);
    ESP.restart();
  }

private:
  uint8_t sr_state_[NUM_SHIFT_REGISTERS];
  unsigned long last_pulse_ms_[SR_CHAIN_BITS];
};
