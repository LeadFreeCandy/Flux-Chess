// FluxChess ESP32-S3 pin assignments
// Derived from PCB18 netlist

// Shift Register Control
pub const PIN_SR_DATA: u8 = 40;
pub const PIN_SR_CLOCK: u8 = 39;
pub const PIN_SR_LATCH: u8 = 42;
pub const PIN_SR_OE: u8 = 48;  // Active low, also NeoPixel on DevKit

// Hall Sensor ADC (BUS0-BUS11)
pub const HALL_PINS: [u8; 12] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
pub const NUM_HALL_SENSORS: usize = 12;

// Buttons (capacitive touch)
pub const PIN_BTN1: u8 = 13;
pub const PIN_BTN2: u8 = 14;

// DC Connector Sense
pub const PIN_DC1: u8 = 38;
pub const PIN_DC2: u8 = 37;

// RGB LED (NeoPixel on DevKit, shared with SR OE)
pub const PIN_RGB_LED: u8 = 48;

// Shift Register Grid
pub const NUM_SHIFT_REGISTERS: usize = 12;
pub const BITS_PER_SR: usize = 5; // Only bits 0-4 drive coils
pub const SR_CHAIN_BITS: usize = NUM_SHIFT_REGISTERS * 8;

// ── ADC type erasure ─────────────────────────────────────────
// GPIO 1-10 are on ADC1, GPIO 11-12 are on ADC2.
// These traits let us store all sensor pins in Vec<Box<dyn ...>>
// without the caller caring which GPIO type is inside.

use esp_hal::analog::adc::{Adc, AdcChannel, AdcPin};
use esp_hal::peripherals::{ADC1, ADC2};
use esp_hal::Blocking;

pub trait Adc1Read {
    fn read(&mut self, adc: &mut Adc<'static, ADC1<'static>, Blocking>) -> u16;
}
impl<P: AdcChannel> Adc1Read for AdcPin<P, ADC1<'static>> {
    fn read(&mut self, adc: &mut Adc<'static, ADC1<'static>, Blocking>) -> u16 {
        adc.read_blocking(self)
    }
}

pub trait Adc2Read {
    fn read(&mut self, adc: &mut Adc<'static, ADC2<'static>, Blocking>) -> u16;
}
impl<P: AdcChannel> Adc2Read for AdcPin<P, ADC2<'static>> {
    fn read(&mut self, adc: &mut Adc<'static, ADC2<'static>, Blocking>) -> u16 {
        adc.read_blocking(self)
    }
}
