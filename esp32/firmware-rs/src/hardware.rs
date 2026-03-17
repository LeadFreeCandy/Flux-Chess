use esp_hal::gpio::{Input, Level, Output, OutputConfig, Pull};
use esp_hal::spi::master::{Config as SpiConfig, Spi};
use esp_hal::spi::Mode as SpiMode;
use esp_hal::time::Rate;
use esp_hal::Blocking;

use crate::pins::*;

const THERMAL_COOLDOWN_MS: u64 = 500;
const SR_SPI_FREQ: u32 = 4_000_000;

pub struct Hardware<'d> {
    spi: Spi<'d, Blocking>,
    latch: Output<'d>,
    oe: Output<'d>,
    // TODO: ADC, buttons, DC sense (need peripheral access)

    sr_state: [u8; NUM_SHIFT_REGISTERS],
    last_pulse_ms: [u64; SR_CHAIN_BITS],
    bit_on_since: [u64; SR_CHAIN_BITS],
}

impl<'d> Hardware<'d> {
    pub fn new(
        spi: Spi<'d, Blocking>,
        latch: Output<'d>,
        oe: Output<'d>,
    ) -> Self {
        let mut hw = Self {
            spi,
            latch,
            oe,
            sr_state: [0u8; NUM_SHIFT_REGISTERS],
            last_pulse_ms: [0u64; SR_CHAIN_BITS],
            bit_on_since: [0u64; SR_CHAIN_BITS],
        };

        // Start with OE disabled and SR blanked
        hw.oe.set_high(); // Active low
        hw.sr_clear();
        log::info!("Hardware init: {} SRs, {} hall sensors", NUM_SHIFT_REGISTERS, NUM_HALL_SENSORS);

        hw
    }

    // ── Shift Registers (SPI) ──────────────────────────────────

    pub fn sr_write(&mut self) {
        // Send last SR first (end of chain shifts out first)
        let mut buf = [0u8; NUM_SHIFT_REGISTERS];
        for i in 0..NUM_SHIFT_REGISTERS {
            buf[i] = self.sr_state[NUM_SHIFT_REGISTERS - 1 - i];
        }
        self.spi.write(&buf).ok();

        // Latch
        self.latch.set_high();
        self.latch.set_low();
    }

    pub fn sr_set_bit(&mut self, bit: usize, val: bool) {
        if bit >= SR_CHAIN_BITS { return; }
        let reg = bit / 8;
        let pos = bit % 8;
        if val {
            self.sr_state[reg] |= 1 << pos;
            if self.bit_on_since[bit] == 0 {
                self.bit_on_since[bit] = now_ms();
                if self.bit_on_since[bit] == 0 { self.bit_on_since[bit] = 1; }
            }
        } else {
            self.sr_state[reg] &= !(1 << pos);
            self.bit_on_since[bit] = 0;
        }
    }

    pub fn sr_clear(&mut self) {
        self.sr_state = [0u8; NUM_SHIFT_REGISTERS];
        self.bit_on_since = [0u64; SR_CHAIN_BITS];
        self.sr_write();
    }

    pub fn sr_set_oe(&mut self, enabled: bool) {
        if enabled {
            self.oe.set_low(); // Active low
        } else {
            self.oe.set_high();
        }
    }

    // ── Coil Pulse ─────────────────────────────────────────────

    pub fn pulse_bit(&mut self, global_bit: usize, duration_ms: u16) -> bool {
        let sr = global_bit / 8;
        let pin = global_bit % 8;

        if global_bit >= SR_CHAIN_BITS {
            log::warn!("pulseBit REJECT: bit {} out of range", global_bit);
            return false;
        }
        if pin >= BITS_PER_SR {
            log::warn!("pulseBit REJECT: SR{} pin {} unused", sr, pin);
            return false;
        }
        if !self.can_pulse(global_bit) {
            let elapsed = now_ms() - self.last_pulse_ms[global_bit];
            let remaining = THERMAL_COOLDOWN_MS - elapsed;
            log::warn!("pulseBit REJECT: SR{} pin {} thermal ({}ms remaining)", sr, pin, remaining);
            return false;
        }

        log::info!("pulseBit START: SR{} pin {} for {}ms", sr, pin, duration_ms);
        log::debug!("  sr_state before: SR{} = 0x{:02X}", sr, self.sr_state[sr]);

        self.sr_set_bit(global_bit, true);

        log::debug!("  sr_state after:  SR{} = 0x{:02X}", sr, self.sr_state[sr]);
        self.sr_write();
        self.sr_set_oe(true);

        // Blocking delay for pulse
        blocking_delay_ms(duration_ms as u32);

        self.sr_set_bit(global_bit, false);
        self.sr_write();
        self.sr_set_oe(false);

        self.last_pulse_ms[global_bit] = now_ms();
        log::info!("pulseBit DONE: SR{} pin {}, cooldown {}ms", sr, pin, THERMAL_COOLDOWN_MS);
        true
    }

    pub fn can_pulse(&self, global_bit: usize) -> bool {
        if global_bit >= SR_CHAIN_BITS { return false; }
        let elapsed = now_ms() - self.last_pulse_ms[global_bit];
        elapsed >= THERMAL_COOLDOWN_MS
    }

    // ── Watchdog check (call periodically) ─────────────────────

    pub fn watchdog_tick(&mut self, max_pulse_ms: u16) {
        let now = now_ms();
        let mut forced = false;
        for bit in 0..SR_CHAIN_BITS {
            if self.bit_on_since[bit] != 0 {
                let on_for = now - self.bit_on_since[bit];
                if on_for > max_pulse_ms as u64 {
                    let reg = bit / 8;
                    let pos = bit % 8;
                    self.sr_state[reg] &= !(1 << pos);
                    self.bit_on_since[bit] = 0;
                    self.last_pulse_ms[bit] = now;
                    forced = true;
                    log::error!("WATCHDOG: force-cleared SR{} pin {} (on for {}ms)", reg, pos, on_for);
                }
            }
        }
        self.sr_write();
        if forced {
            self.sr_set_oe(false);
        }
    }

    // ── Hall Sensors ──────────────────────────────────────────

    // TODO: ADC reads require peripheral access - will be added when
    // we wire up the ADC peripheral in main
    pub fn read_all_sensors(&self) -> [u16; NUM_HALL_SENSORS] {
        // Stub — return zeros until ADC is wired
        [0u16; NUM_HALL_SENSORS]
    }

    // ── RGB LED ────────────────────────────────────────────────

    pub fn set_rgb(&mut self, _r: u8, _g: u8, _b: u8) {
        log::info!("setRGB({}, {}, {})", _r, _g, _b);
        // NeoPixel requires RMT peripheral — TODO
        // For now just clear SR to be safe (shared pin)
        self.sr_clear();
    }

    // ── Shutdown ──────────────────────────────────────────────

    pub fn shutdown(&mut self) -> ! {
        log::info!("shutdown: blanking SR, disabling OE, restarting");
        self.sr_clear();
        self.sr_set_oe(false);
        blocking_delay_ms(50);
        esp_hal::system::software_reset();
        loop {} // never reached
    }
}

// ── Time helpers ──────────────────────────────────────────────

fn now_ms() -> u64 {
    esp_hal::time::Instant::now()
        .duration_since_epoch()
        .as_millis()
}

fn blocking_delay_ms(ms: u32) {
    let delay = esp_hal::delay::Delay::new();
    delay.delay_millis(ms);
}
