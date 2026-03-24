#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "pins.h"
#include "utils.h"

// Minimum milliseconds between pulses on the same coil
#define THERMAL_COOLDOWN_MS 500

// SPI clock for shift registers (74HC595 max ~25MHz, use 4MHz for safety)
#define SR_SPI_FREQ 500000  // 500kHz — reduced for long PCB traces
/* #define SR_SPI_FREQ SPI_CLK_SRC_DEFAULT */

// Watchdog interval and max pulse enforcement
#define WATCHDOG_INTERVAL_MS 100

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
    memset(bit_on_since_, 0, sizeof(bit_on_since_));

    // Create mutex
    sr_mutex_ = xSemaphoreCreateMutex();

    // Blank shift registers
    srClear();

    // Start watchdog thread
    xTaskCreatePinnedToCore(
      watchdogTask, "hw_watchdog", 4096, this, 1, &watchdog_handle_, 1
    );

    LOG_HW("init: SPI CLK=%d DATA=%d, %d SRs, %d sensors, watchdog started", PIN_SR_CLOCK, PIN_SR_DATA, NUM_SHIFT_REGISTERS, NUM_HALL_SENSORS);
  }

  // ── Shift Registers (SPI) ──────────────────────────────────

  void srSetBit(uint8_t bit, bool val) {
    if (bit >= SR_CHAIN_BITS) return;
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    uint8_t reg = bit / 8;
    uint8_t pos = bit % 8;
    if (val) {
      sr_state_[reg] |= (1 << pos);
      if (bit_on_since_[bit] == 0) {
        bit_on_since_[bit] = millis();
        if (bit_on_since_[bit] == 0) bit_on_since_[bit] = 1; // avoid 0 meaning "off"
      }
    } else {
      sr_state_[reg] &= ~(1 << pos);
      bit_on_since_[bit] = 0;
    }
    xSemaphoreGive(sr_mutex_);
  }

  void srClear() {
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    memset(sr_state_, 0, sizeof(sr_state_));
    memset(bit_on_since_, 0, sizeof(bit_on_since_));
    xSemaphoreGive(sr_mutex_);
    srWriteInternal();
  }

  void srSetOE(bool enabled) {
    digitalWrite(PIN_SR_OE, enabled ? LOW : HIGH);
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

  // Sets a coil bit high for duration_ms then clears it.
  // The watchdog thread writes sr_state_ to hardware every 100ms
  // and enforces max pulse duration as a safety net.
  bool pulseBit(uint8_t globalBit, uint16_t duration_ms) {
    uint8_t sr = globalBit / 8;
    uint8_t pin = globalBit % 8;

    if (globalBit >= SR_CHAIN_BITS) {
      LOG_HW("pulseBit REJECT: bit %d out of range (max %d)", globalBit, SR_CHAIN_BITS - 1);
      return false;
    }
    if (pin >= BITS_PER_SR) {
      LOG_HW("pulseBit REJECT: SR%d pin %d is unused (only pins 0-%d connected)", sr, pin, BITS_PER_SR - 1);
      return false;
    }
    if (!canPulse(globalBit)) {
      unsigned long elapsed = millis() - last_pulse_ms_[globalBit];
      unsigned long remaining = THERMAL_COOLDOWN_MS - elapsed;
      LOG_HW("pulseBit REJECT: SR%d pin %d thermal cooldown (%lums remaining of %dms)", sr, pin, remaining, THERMAL_COOLDOWN_MS);
      return false;
    }

    LOG_HW("pulseBit START: SR%d pin %d (global bit %d) for %dms", sr, pin, globalBit, duration_ms);
    srSetBit(globalBit, true);
    srSetOE(true);

    // Wait for pulse duration — watchdog keeps writing sr_state_ to hw
    delay(duration_ms);

    srSetBit(globalBit, false);
    srSetOE(false);

    // Record for thermal tracking
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    last_pulse_ms_[globalBit] = millis();
    xSemaphoreGive(sr_mutex_);

    LOG_HW("pulseBit DONE: SR%d pin %d, next allowed in %dms", sr, pin, THERMAL_COOLDOWN_MS);
    return true;
  }

  bool canPulse(uint8_t globalBit) {
    if (globalBit >= SR_CHAIN_BITS) return false;
    unsigned long elapsed = millis() - last_pulse_ms_[globalBit];
    bool ok = elapsed >= THERMAL_COOLDOWN_MS;
    if (ok) {
      LOG_HW("thermal check: bit %d OK (idle %lums)", globalBit, elapsed);
    }
    return ok;
  }

  // ── RGB LED ────────────────────────────────────────────────

  void setRGB(uint8_t r, uint8_t g, uint8_t b) {
    LOG_HW("setRGB(%d, %d, %d)", r, g, b);
    srClear();
    neopixelWrite(PIN_RGB_LED, r, g, b);
  }

  // ── Shutdown ──────────────────────────────────────────────

  void shutdown() {
    LOG_HW("shutdown: stopping watchdog, blanking SR, disabling OE");
    if (watchdog_handle_) {
      vTaskDelete(watchdog_handle_);
      watchdog_handle_ = nullptr;
    }
    srClear();
    srSetOE(false);
    delay(50);
    ESP.restart();
  }

private:
  SPIClass spi_;
  uint8_t sr_state_[NUM_SHIFT_REGISTERS];
  unsigned long last_pulse_ms_[SR_CHAIN_BITS];
  unsigned long bit_on_since_[SR_CHAIN_BITS];  // 0 = off, else millis() when turned on
  SemaphoreHandle_t sr_mutex_;
  TaskHandle_t watchdog_handle_ = nullptr;

  // Write sr_state_ to hardware (no mutex, caller must hold or be watchdog)
  void srWriteInternal() {
    spi_.beginTransaction(SPISettings(SR_SPI_FREQ, MSBFIRST, SPI_MODE0));
    for (int i = NUM_SHIFT_REGISTERS - 1; i >= 0; i--) {
      spi_.transfer(sr_state_[i]);
    }
    spi_.endTransaction();
    digitalWrite(PIN_SR_LATCH, HIGH);
    digitalWrite(PIN_SR_LATCH, LOW);
  }

  // ── Watchdog Thread ───────────────────────────────────────
  // Runs every 100ms:
  //  1. Writes sr_state_ to shift registers
  //  2. Checks any bits that have been high longer than MAX_PULSE_MS
  //     and force-clears them as a safety net

  static void watchdogTask(void* param) {
    Hardware* hw = static_cast<Hardware*>(param);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(WATCHDOG_INTERVAL_MS));

      xSemaphoreTake(hw->sr_mutex_, portMAX_DELAY);

      // Enforce max pulse duration
      unsigned long now = millis();
      bool forced_clear = false;
      for (int bit = 0; bit < SR_CHAIN_BITS; bit++) {
        if (hw->bit_on_since_[bit] != 0) {
          unsigned long on_for = now - hw->bit_on_since_[bit];
          if (on_for > MAX_PULSE_MS) {
            uint8_t reg = bit / 8;
            uint8_t pos = bit % 8;
            hw->sr_state_[reg] &= ~(1 << pos);
            hw->bit_on_since_[bit] = 0;
            hw->last_pulse_ms_[bit] = now;
            forced_clear = true;
            LOG_HW("WATCHDOG: force-cleared SR%d pin %d (on for %lums, max %dms)", reg, pos, on_for, MAX_PULSE_MS);
          }
        }
      }

      // Write current state to hardware
      hw->srWriteInternal();

      xSemaphoreGive(hw->sr_mutex_);

      if (forced_clear) {
        hw->srSetOE(false);
        LOG_HW("WATCHDOG: OE disabled after force-clear");
      }
    }
  }
};
