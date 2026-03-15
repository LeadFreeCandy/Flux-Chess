#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "pins.h"

// Minimum milliseconds between pulses on the same coil
#define THERMAL_COOLDOWN_MS 500

// SPI clock for shift registers (74HC595 max ~25MHz, use 4MHz for safety)
#define SR_SPI_FREQ 4000000

class Hardware {
public:
  Hardware() : spi_(FSPI) {
    // SPI for shift registers: MOSI=DATA, CLK=CLOCK, no MISO
    spi_.begin(PIN_SR_CLOCK, -1, PIN_SR_DATA, -1);

    // Latch and OE are manual GPIO
    pinMode(PIN_SR_LATCH, OUTPUT);
    digitalWrite(PIN_SR_LATCH, LOW);
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

  // ── Shift Registers (SPI) ──────────────────────────────────

  void srWrite() {
    spi_.beginTransaction(SPISettings(SR_SPI_FREQ, MSBFIRST, SPI_MODE0));
    // Send last SR first (end of chain shifts out first)
    for (int i = NUM_SHIFT_REGISTERS - 1; i >= 0; i--) {
      spi_.transfer(sr_state_[i]);
    }
    spi_.endTransaction();
    // Latch: pulse high to transfer shift register to output
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

  // ── Coil Pulse ─────────────────────────────────────────────

  static const uint8_t BITS_PER_SR = 5;  // Only bits 0-4 drive coils

  // Returns false if invalid bit, or thermal limit prevents pulse
  bool pulseBit(uint8_t globalBit, uint16_t duration_ms) {
    if (globalBit >= SR_CHAIN_BITS) return false;
    // Reject bits 5-7 within each SR (unused/not connected)
    if ((globalBit % 8) >= BITS_PER_SR) return false;
    if (!canPulse(globalBit)) return false;

    srSetBit(globalBit, true);
    srWrite();
    srSetOE(true);

    delay(duration_ms);

    srSetBit(globalBit, false);
    srWrite();
    srSetOE(false);

    recordPulse(globalBit);
    return true;
  }

  bool canPulse(uint8_t globalBit) {
    if (globalBit >= SR_CHAIN_BITS) return false;
    return (millis() - last_pulse_ms_[globalBit]) >= THERMAL_COOLDOWN_MS;
  }

  // ── Shutdown ──────────────────────────────────────────────

  void shutdown() {
    srClear();
    srSetOE(false);
    delay(50);
    ESP.restart();
  }

private:
  SPIClass spi_;
  uint8_t sr_state_[NUM_SHIFT_REGISTERS];
  unsigned long last_pulse_ms_[SR_CHAIN_BITS];

  void recordPulse(uint8_t globalBit) {
    if (globalBit >= SR_CHAIN_BITS) return;
    last_pulse_ms_[globalBit] = millis();
  }
};
