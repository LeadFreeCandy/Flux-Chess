#pragma once

// FluxChess ESP32-S3 pin definitions
// Derived from PCB18 netlist (Netlist_PCB18_2026-03-10.tel)
// ESP32-S3-DevKitC-1 (U17)

// ── Shift Register Control ──────────────────────────────────
// 12 daisy-chained 74HC595 shift registers (U16→U12→U22→...→U85)
// Each SR has 4 outputs driving coil H-bridge MOSFETs

// OLD PIN CONNECTIONS
/* #define PIN_SR_DATA   40   // U17.37 → U16.14 (SER input to first SR) */
/* #define PIN_SR_CLOCK  39   // U17.36 → all SR pin 11 (SRCLK) */
/* #define PIN_SR_LATCH  42   // U17.39 → all SR pin 12 (RCLK) */
/* #define PIN_SR_OE     48   // U17.29 → all SR pin 13 (active low, accent accent pulled high via R53) */

// FIXED PIN CONNECTIONS 
#define PIN_SR_DATA   45   // U17.37 → U16.14 (SER input to first SR)
#define PIN_SR_CLOCK  0   // U17.36 → all SR pin 11 (SRCLK)
#define PIN_SR_LATCH  47   // U17.39 → all SR pin 12 (RCLK)
#define PIN_SR_OE     41   // U17.29 → all SR pin 13 (active low, accent accent pulled high via R53)

// ── Hall Sensor ADC Inputs ──────────────────────────────────
// 12 Hall sensors connected via 0Ω bridge resistors to BUS lines

#define NUM_HALL_SENSORS 12

#define PIN_BUS0   1    // U17.41 → R39 → Hall U13
#define PIN_BUS1   2    // U17.40 → R26 → Hall U1
#define PIN_BUS2   3    // U17.13 → R17 → Hall U6
#define PIN_BUS3   4    // U17.4  → R79 → Hall U38
#define PIN_BUS4   5    // U17.5  → R66 → Hall U31
#define PIN_BUS5   6    // U17.6  → R52 → Hall U24
#define PIN_BUS6   7    // U17.7  → R118 → Hall U59
#define PIN_BUS7   8    // U17.12 → R105 → Hall U52
#define PIN_BUS8   9    // U17.15 → R92 → Hall U45
#define PIN_BUS9   10   // U17.16 → R157 → Hall U80
#define PIN_BUS10  11   // U17.17 → R144 → Hall U73
#define PIN_BUS11  12   // U17.18 → R131 → Hall U66

static const uint8_t HALL_PINS[NUM_HALL_SENSORS] = {
    PIN_BUS0, PIN_BUS1, PIN_BUS2,  PIN_BUS3,  PIN_BUS4,  PIN_BUS5,
    PIN_BUS6, PIN_BUS7, PIN_BUS8,  PIN_BUS9,  PIN_BUS10, PIN_BUS11,
};

// ── Buttons ─────────────────────────────────────────────────

#define PIN_BTN1  13   // U17.19 → U87
#define PIN_BTN2  14   // U17.20 → U91

// ── DC Connector Sense ──────────────────────────────────────

#define PIN_DC1   38   // U17.35 → DC1.2
#define PIN_DC2   37   // U17.34 → DC2.2

// ── RGB LED ───────────────────────────────────────────────────

#define PIN_RGB_LED  48   // Onboard NeoPixel

// ── Shift Register Grid Mapping ─────────────────────────────
// Maps grid position [row][col] to shift register bit index
// 12 SRs × 4 outputs = 96 total bits
// SR chain order: U16→U12→U22→U29→U36→U43→U50→U57→U64→U71→U78→U85
// Bits 0-3 = U16 (Q0-Q3), bits 4-7 = U12, ..., bits 92-95 = U85

#define NUM_SHIFT_REGISTERS  12
#define SR_CHAIN_BITS        (NUM_SHIFT_REGISTERS * 8)
