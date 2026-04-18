#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "pins.h"
#include "utils.h"

// Thermal model: heat accumulates with each pulse, decays over time
// COOL_RATE: ms of heat dissipated per ms of real time (0.5 = cools half as fast as it heats)
// MAX_HEAT_MS: reject pulse if accumulated heat would exceed this
#define THERMAL_COOL_RATE  0.5f
#define THERMAL_MAX_HEAT_MS 1500

// SPI clock for shift registers (74HC595 max ~25MHz, use 4MHz for safety)
#define SR_SPI_FREQ 500000  // 500kHz — reduced for long PCB traces

// Watchdog interval and max pulse enforcement
#define WATCHDOG_INTERVAL_MS 100

class Hardware {
public:
  Hardware() : spi_(FSPI) {
    spi_.begin(PIN_SR_CLOCK, -1, PIN_SR_DATA, -1);

    pinMode(PIN_SR_LATCH, OUTPUT);
    digitalWrite(PIN_SR_LATCH, LOW);
    pinMode(PIN_SR_OE, OUTPUT);
    digitalWrite(PIN_SR_OE, HIGH);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    for (int i = 0; i < NUM_HALL_SENSORS; i++) {
      pinMode(HALL_PINS[i], INPUT);
    }

    analogWriteFrequency(PIN_SR_OE, 20000); // 20kHz — just above audible range

    pinMode(PIN_DC1, INPUT);
    pinMode(PIN_DC2, INPUT);

    memset(sr_state_, 0, sizeof(sr_state_));
    memset(heat_ms_, 0, sizeof(heat_ms_));
    memset(heat_updated_, 0, sizeof(heat_updated_));

    sr_mutex_ = xSemaphoreCreateMutex();
    srClear();

    xTaskCreatePinnedToCore(
      watchdogTask, "hw_watchdog", 4096, this, 1, &watchdog_handle_, 1
    );

    LOG_HW("init: SPI CLK=%d DATA=%d, %d SRs, %d sensors, watchdog started",
            PIN_SR_CLOCK, PIN_SR_DATA, NUM_SHIFT_REGISTERS, NUM_HALL_SENSORS);
  }

  // ── PWM Frequency ──────────────────────────────────────────

  void setPwmFrequency(uint16_t freq_hz) {
    analogWriteFrequency(PIN_SR_OE, freq_hz);
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

  // ── Coil Control (safe public API) ────────────────────────

  static const uint8_t BITS_PER_SR = 5;

  // Pulse a coil for a fixed duration. Returns false if rejected (thermal/invalid).
  bool pulseBit(uint8_t globalBit, uint16_t duration_ms, uint8_t pwm_duty = 255) {
    if (!validateBit(globalBit)) return false;

    float heat = decayHeat(globalBit);
    if (heat + duration_ms > THERMAL_MAX_HEAT_MS) {
      LOG_HW("pulseBit REJECT: bit %d thermal (heat=%.0f + %d > %d)",
             globalBit, heat, duration_ms, THERMAL_MAX_HEAT_MS);
      return false;
    }

    srSetBit(globalBit, true);
    srSetPWM(pwm_duty);
    delay(duration_ms);
    srSetPWM(0);
    srSetBit(globalBit, false);

    heat_ms_[globalBit] = heat + duration_ms * (pwm_duty / 255.0f);
    heat_updated_[globalBit] = millis();
    return true;
  }


  // Play a musical note on a specific coil
  bool playNote(uint8_t globalBit, uint16_t freq_hz, uint16_t duration_ms, uint8_t volume_duty = 128) {
    if (!validateBit(globalBit)) return false;

    // 1. Thermal check (notes generate heat just like moves)
    float heat = decayHeat(globalBit);
    if (heat + duration_ms > THERMAL_MAX_HEAT_MS) {
      LOG_HW("playNote REJECT: bit %d thermal limit", globalBit);
      return false;
    }

    // 2. Set PWM to the audible musical frequency
    analogWriteFrequency(PIN_SR_OE, freq_hz);

    // 3. Turn on the specific coil with the duty cycle (volume)
    srSetBit(globalBit, true);
    srSetPWM(volume_duty);
    
    // 4. Hold the note
    delay(duration_ms);

    // 5. Turn off the coil
    srSetPWM(0);
    srSetBit(globalBit, false);

    // 6. RESTORE to silent ultrasonic frequency for standard moves!
    analogWriteFrequency(PIN_SR_OE, 20000);

    // 7. Record the heat generated
    heat_ms_[globalBit] = heat + duration_ms * (volume_duty / 255.0f);
    heat_updated_[globalBit] = millis();
    
    return true;
  }



  // Activate a coil and leave it on (for use with sustainCoil).
  // Clears all bits, sets the requested bit, flushes to hardware.
  bool startCoil(uint8_t globalBit, uint8_t pwm_duty = 255) {
    if (!validateBit(globalBit)) return false;

    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    memset(sr_state_, 0, sizeof(sr_state_));

    uint8_t reg = globalBit / 8;
    uint8_t pos = globalBit % 8;
    sr_state_[reg] |= (1 << pos);

    srWriteInternal();
    xSemaphoreGive(sr_mutex_);

    srSetPWM(pwm_duty);
    return true;
  }

  // Activate multiple coils simultaneously. Clears all bits, sets requested bits, flushes.
  bool startCoils(const uint8_t* bits, int count, uint8_t pwm_duty = 255) {
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    memset(sr_state_, 0, sizeof(sr_state_));
    for (int i = 0; i < count; i++) {
      if (!validateBit(bits[i])) { xSemaphoreGive(sr_mutex_); return false; }
      uint8_t reg = bits[i] / 8;
      uint8_t pos = bits[i] % 8;
      sr_state_[reg] |= (1 << pos);
    }
    srWriteInternal();
    xSemaphoreGive(sr_mutex_);
    srSetPWM(pwm_duty);
    return true;
  }

  // Stop all coils and flush.
  void stopAllCoils() {
    srSetPWM(0);
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    memset(sr_state_, 0, sizeof(sr_state_));
    srWriteInternal();
    xSemaphoreGive(sr_mutex_);
  }

  // Turn off the active coil and flush to hardware.
  void stopCoil(uint8_t globalBit) {
    srSetPWM(0);

    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    uint8_t reg = globalBit / 8;
    uint8_t pos = globalBit % 8;
    sr_state_[reg] &= ~(1 << pos);
    srWriteInternal();
    xSemaphoreGive(sr_mutex_);
  }

  // Sustain the currently active coil without SPI writes.
  // Must be the same bit as the last pulseBit/sustainCoil call.
  // Returns false if bit mismatch or validation fails.
  bool sustainCoil(uint8_t globalBit, uint16_t duration_us, uint8_t pwm_duty = 255) {
    if (!validateBit(globalBit)) return false;

    // Verify this bit is currently active
    uint8_t reg = globalBit / 8;
    uint8_t pos = globalBit % 8;
    if (!(sr_state_[reg] & (1 << pos))) {
      LOG_HW("sustainCoil REJECT: bit %d not active", globalBit);
      return false;
    }

    srSetPWM(pwm_duty);
    delayMicroseconds(duration_us);

    return true;
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
  float heat_ms_[SR_CHAIN_BITS];
  unsigned long heat_updated_[SR_CHAIN_BITS];
  SemaphoreHandle_t sr_mutex_;
  TaskHandle_t watchdog_handle_ = nullptr;

  // ── Validation ──────────────────────────────────────────────

  bool validateBit(uint8_t globalBit) {
    if (globalBit >= SR_CHAIN_BITS) {
      LOG_HW("REJECT: bit %d out of range", globalBit);
      return false;
    }
    uint8_t pin = globalBit % 8;
    if (pin >= BITS_PER_SR) {
      LOG_HW("REJECT: bit %d pin %d unused", globalBit, pin);
      return false;
    }
    return true;
  }

  // ── Thermal ─────────────────────────────────────────────────

  float decayHeat(uint8_t bit) {
    unsigned long now = millis();
    unsigned long dt = now - heat_updated_[bit];
    float h = heat_ms_[bit] - dt * THERMAL_COOL_RATE;
    if (h < 0) h = 0;
    heat_ms_[bit] = h;
    heat_updated_[bit] = now;
    return h;
  }

  // ── Shift Register Internals ────────────────────────────────

  void srSetBit(uint8_t bit, bool val) {
    if (bit >= SR_CHAIN_BITS) return;
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    uint8_t reg = bit / 8;
    uint8_t pos = bit % 8;
    if (val) {
      sr_state_[reg] |= (1 << pos);
    } else {
      sr_state_[reg] &= ~(1 << pos);
    }
    xSemaphoreGive(sr_mutex_);
  }

  void srClear() {
    xSemaphoreTake(sr_mutex_, portMAX_DELAY);
    memset(sr_state_, 0, sizeof(sr_state_));
    xSemaphoreGive(sr_mutex_);
    srWriteInternal();
  }

  void srSetOE(bool enabled) {
    digitalWrite(PIN_SR_OE, enabled ? LOW : HIGH);
  }

  void srSetPWM(uint8_t duty) {
    if (duty == 0) {
      analogWrite(PIN_SR_OE, 255);
    } else if (duty >= 255) {
      analogWrite(PIN_SR_OE, 0);
    } else {
      analogWrite(PIN_SR_OE, 255 - duty);
    }
  }

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

  // Watchdog: periodically flushes sr_state_ to hardware.
  // Thermal protection is handled by decayHeat in pulseBit/startCoil.
  static void watchdogTask(void* param) {
    Hardware* hw = static_cast<Hardware*>(param);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(WATCHDOG_INTERVAL_MS));

      xSemaphoreTake(hw->sr_mutex_, portMAX_DELAY);
      hw->srWriteInternal();
      xSemaphoreGive(hw->sr_mutex_);
    }
  }
};
