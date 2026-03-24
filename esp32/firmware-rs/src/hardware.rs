extern crate alloc;
use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;

use esp_hal::analog::adc::{Adc, AdcConfig, Attenuation};
use esp_hal::gpio::{Level, Output, OutputConfig};
use esp_hal::peripherals::*;
use esp_hal::spi::master::Spi;
use esp_hal::Blocking;

use crate::api::*;
use crate::pins::*;

const THERMAL_COOLDOWN_US: u64 = 500_000; // 500ms in micros

pub struct Hardware {
    spi: Spi<'static, Blocking>,
    latch: Output<'static>,
    oe: Output<'static>,

    adc1: Adc<'static, ADC1<'static>, Blocking>,
    adc2: Adc<'static, ADC2<'static>, Blocking>,
    adc1_pins: Vec<Box<dyn Adc1Read>>,
    adc2_pins: Vec<Box<dyn Adc2Read>>,

    sr_state: [u8; NUM_SHIFT_REGISTERS],
    last_pulse_us: [u64; SR_CHAIN_BITS],
    bit_on_since_us: [u64; SR_CHAIN_BITS],
}

impl Hardware {
    pub fn new(
        spi: Spi<'static, Blocking>,
        latch: Output<'static>,
        oe: Output<'static>,
        adc1_periph: ADC1<'static>,
        adc2_periph: ADC2<'static>,
        g1: GPIO1<'static>, g2: GPIO2<'static>, g3: GPIO3<'static>,
        g4: GPIO4<'static>, g5: GPIO5<'static>, g6: GPIO6<'static>,
        g7: GPIO7<'static>, g8: GPIO8<'static>, g9: GPIO9<'static>,
        g10: GPIO10<'static>, g11: GPIO11<'static>, g12: GPIO12<'static>,
    ) -> Self {
        let a = Attenuation::_11dB;

        let mut c1 = AdcConfig::new();
        let adc1_pins: Vec<Box<dyn Adc1Read>> = vec![
            Box::new(c1.enable_pin(g1, a)),  Box::new(c1.enable_pin(g2, a)),
            Box::new(c1.enable_pin(g3, a)),  Box::new(c1.enable_pin(g4, a)),
            Box::new(c1.enable_pin(g5, a)),  Box::new(c1.enable_pin(g6, a)),
            Box::new(c1.enable_pin(g7, a)),  Box::new(c1.enable_pin(g8, a)),
            Box::new(c1.enable_pin(g9, a)),  Box::new(c1.enable_pin(g10, a)),
        ];
        let adc1 = Adc::new(adc1_periph, c1);

        let mut c2 = AdcConfig::new();
        let adc2_pins: Vec<Box<dyn Adc2Read>> = vec![
            Box::new(c2.enable_pin(g11, a)), Box::new(c2.enable_pin(g12, a)),
        ];
        let adc2 = Adc::new(adc2_periph, c2);

        let mut hw = Self {
            spi, latch, oe, adc1, adc2, adc1_pins, adc2_pins,
            sr_state: [0u8; NUM_SHIFT_REGISTERS],
            last_pulse_us: [0u64; SR_CHAIN_BITS],
            bit_on_since_us: [0u64; SR_CHAIN_BITS],
        };

        hw.sr_set_oe(false);
        hw.sr_clear();

        // Disable USB-OTG pull resistors on GPIO 1/2 (D-/D+)
        // Without this, ADC channels 0-1 read ~700 instead of true analog value
        let usb_wrap = unsafe { &*esp32s3::USB_WRAP::ptr() };
        usb_wrap.otg_conf().modify(|_, w| {
            w.pad_pull_override().set_bit()
             .dp_pulldown().clear_bit()
             .dm_pulldown().clear_bit()
             .dp_pullup().clear_bit()
             .dm_pullup().clear_bit()
        });

        log::info!("Hardware init: {} SRs, {} sensors", NUM_SHIFT_REGISTERS, NUM_HALL_SENSORS);
        hw
    }

    // ── Hall Sensors ──────────────────────────────────────────

    pub fn read_sensor(&mut self, index: u8) -> u16 {
        let i = index as usize;
        if i >= NUM_HALL_SENSORS { return 0; }
        if i < self.adc1_pins.len() {
            self.adc1_pins[i].read(&mut self.adc1)
        } else {
            self.adc2_pins[i - self.adc1_pins.len()].read(&mut self.adc2)
        }
    }

    pub fn read_all_sensors(&mut self) -> SensorGrid<u16> {
        let mut grid = [[0u16; SENSOR_ROWS]; SENSOR_COLS];
        for i in 0..NUM_HALL_SENSORS {
            let col = i % SENSOR_COLS;
            let row = i / SENSOR_COLS;
            grid[col][row] = self.read_sensor(i as u8);
        }
        grid
    }

    // ── Coil Pulse ─────────────────────────────────────────────

    pub fn pulse_bit(&mut self, global_bit: usize, duration_us: u32) -> PulseResult {
        let result = self.validate_pulse_bit(global_bit, duration_us);
        if let PulseResult::Failure(_) = result {
            return result;
        }

        let sr = global_bit / 8;
        let pin = global_bit % 8;
        log::info!("pulseBit START: SR{} pin {} for {}us", sr, pin, duration_us);

        self.sr_set_bit(global_bit, true);
        self.sr_write();
        self.sr_set_oe(true);

        blocking_delay_us(duration_us);

        self.sr_set_bit(global_bit, false);
        self.sr_write();
        self.sr_set_oe(false);

        self.last_pulse_us[global_bit] = now_us();
        log::info!("pulseBit DONE: SR{} pin {}, cooldown {}us", sr, pin, THERMAL_COOLDOWN_US);

        PulseResult::Success
    }

    pub fn validate_pulse_bit(&self, global_bit: usize, duration_us: u32) -> PulseResult {
        if global_bit >= SR_CHAIN_BITS {
            return PulseResult::Failure(PulseError::InvalidCoil);
        }
        if now_us() - self.last_pulse_us[global_bit] < THERMAL_COOLDOWN_US {
            return PulseResult::Failure(PulseError::ThermalLimit);
        }
        if duration_us > MAX_PULSE_US {
            return PulseResult::Failure(PulseError::PulseTooLong);
        }
        PulseResult::Success
    }

    // ── Shift Registers (SPI) ──────────────────────────────────

    fn sr_write(&mut self) {
        let mut buf = [0u8; NUM_SHIFT_REGISTERS];
        for i in 0..NUM_SHIFT_REGISTERS {
            buf[i] = self.sr_state[NUM_SHIFT_REGISTERS - 1 - i];
        }
        self.spi.write(&buf).ok();
        self.latch.set_high();
        self.latch.set_low();
    }

    fn sr_set_bit(&mut self, bit: usize, val: bool) {
        if bit >= SR_CHAIN_BITS { return; }
        let reg = bit / 8;
        let pos = bit % 8;
        if val {
            self.sr_state[reg] |= 1 << pos;
            if self.bit_on_since_us[bit] == 0 {
                self.bit_on_since_us[bit] = now_us();
                if self.bit_on_since_us[bit] == 0 { self.bit_on_since_us[bit] = 1; }
            }
        } else {
            self.sr_state[reg] &= !(1 << pos);
            self.bit_on_since_us[bit] = 0;
        }
    }

    fn sr_clear(&mut self) {
        self.sr_state = [0u8; NUM_SHIFT_REGISTERS];
        self.bit_on_since_us = [0u64; SR_CHAIN_BITS];
        self.sr_write();
    }

    fn sr_set_oe(&mut self, enabled: bool) {
        if enabled { self.oe.set_low(); } else { self.oe.set_high(); }
    }

    // ── Watchdog ──────────────────────────────────────────────

    pub fn watchdog_tick(&mut self, max_pulse_us: u32) {
        let now = now_us();
        let mut forced = false;
        for bit in 0..SR_CHAIN_BITS {
            if self.bit_on_since_us[bit] != 0 {
                let on_for = now - self.bit_on_since_us[bit];
                if on_for > max_pulse_us as u64 {
                    let reg = bit / 8;
                    let pos = bit % 8;
                    self.sr_state[reg] &= !(1 << pos);
                    self.bit_on_since_us[bit] = 0;
                    self.last_pulse_us[bit] = now;
                    forced = true;
                    log::error!("WATCHDOG: force-cleared SR{} pin {} (on for {}us)", reg, pos, on_for);
                }
            }
        }
        self.sr_write();
        if forced { self.sr_set_oe(false); }
    }

    // ── RGB LED ────────────────────────────────────────────────

    pub fn set_rgb(&mut self, _r: u8, _g: u8, _b: u8) {
        log::info!("setRGB({}, {}, {})", _r, _g, _b);
        self.sr_clear();
    }

    // ── Shutdown ──────────────────────────────────────────────

    pub fn shutdown(&mut self) -> ! {
        log::info!("shutdown: blanking SR, disabling OE, restarting");
        self.sr_clear();
        self.sr_set_oe(false);
        blocking_delay_us(50_000);
        esp_hal::system::software_reset();
        loop {}
    }
}

fn now_us() -> u64 {
    esp_hal::time::Instant::now().duration_since_epoch().as_micros()
}

pub fn blocking_delay_us(us: u32) {
    esp_hal::delay::Delay::new().delay_micros(us);
}
