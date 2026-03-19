use esp_hal::analog::adc::{Adc, AdcConfig, AdcPin, Attenuation};
use esp_hal::gpio::{Level, Output, OutputConfig};
use esp_hal::peripherals::*;
use esp_hal::spi::master::Spi;
use esp_hal::Blocking;

use crate::pins::*;

const THERMAL_COOLDOWN_MS: u64 = 500;

pub struct SensorPins {
    s0:  AdcPin<GPIO1<'static>,  ADC1<'static>>,
    s1:  AdcPin<GPIO2<'static>,  ADC1<'static>>,
    s2:  AdcPin<GPIO3<'static>,  ADC1<'static>>,
    s3:  AdcPin<GPIO4<'static>,  ADC1<'static>>,
    s4:  AdcPin<GPIO5<'static>,  ADC1<'static>>,
    s5:  AdcPin<GPIO6<'static>,  ADC1<'static>>,
    s6:  AdcPin<GPIO7<'static>,  ADC1<'static>>,
    s7:  AdcPin<GPIO8<'static>,  ADC1<'static>>,
    s8:  AdcPin<GPIO9<'static>,  ADC1<'static>>,
    s9:  AdcPin<GPIO10<'static>, ADC1<'static>>,
    s10: AdcPin<GPIO11<'static>, ADC2<'static>>,
    s11: AdcPin<GPIO12<'static>, ADC2<'static>>,
}

pub struct Hardware {
    spi: Spi<'static, Blocking>,
    latch: Output<'static>,
    oe: Output<'static>,

    adc1: Adc<'static, ADC1<'static>, Blocking>,
    adc2: Adc<'static, ADC2<'static>, Blocking>,
    sensors: SensorPins,

    sr_state: [u8; NUM_SHIFT_REGISTERS],
    last_pulse_ms: [u64; SR_CHAIN_BITS],
    bit_on_since: [u64; SR_CHAIN_BITS],
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
        let s0  = c1.enable_pin(g1, a);
        let s1  = c1.enable_pin(g2, a);
        let s2  = c1.enable_pin(g3, a);
        let s3  = c1.enable_pin(g4, a);
        let s4  = c1.enable_pin(g5, a);
        let s5  = c1.enable_pin(g6, a);
        let s6  = c1.enable_pin(g7, a);
        let s7  = c1.enable_pin(g8, a);
        let s8  = c1.enable_pin(g9, a);
        let s9  = c1.enable_pin(g10, a);
        let adc1 = Adc::new(adc1_periph, c1);

        let mut c2 = AdcConfig::new();
        let s10 = c2.enable_pin(g11, a);
        let s11 = c2.enable_pin(g12, a);
        let adc2 = Adc::new(adc2_periph, c2);

        let mut hw = Self {
            spi, latch, oe, adc1, adc2,
            sensors: SensorPins { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11 },
            sr_state: [0u8; NUM_SHIFT_REGISTERS],
            last_pulse_ms: [0u64; SR_CHAIN_BITS],
            bit_on_since: [0u64; SR_CHAIN_BITS],
        };

        hw.sr_set_oe(false);
        hw.sr_clear();
        log::info!("Hardware init: {} SRs, {} sensors", NUM_SHIFT_REGISTERS, NUM_HALL_SENSORS);
        hw
    }

    // ── Hall Sensors ──────────────────────────────────────────

    pub fn read_sensor(&mut self, index: u8) -> u16 {
        let s = &mut self.sensors;
        match index {
            0  => self.adc1.read_blocking(&mut s.s0),
            1  => self.adc1.read_blocking(&mut s.s1),
            2  => self.adc1.read_blocking(&mut s.s2),
            3  => self.adc1.read_blocking(&mut s.s3),
            4  => self.adc1.read_blocking(&mut s.s4),
            5  => self.adc1.read_blocking(&mut s.s5),
            6  => self.adc1.read_blocking(&mut s.s6),
            7  => self.adc1.read_blocking(&mut s.s7),
            8  => self.adc1.read_blocking(&mut s.s8),
            9  => self.adc1.read_blocking(&mut s.s9),
            10 => self.adc2.read_blocking(&mut s.s10),
            11 => self.adc2.read_blocking(&mut s.s11),
            _  => 0,
        }
    }

    pub fn read_all_sensors(&mut self) -> [u16; NUM_HALL_SENSORS] {
        let mut values = [0u16; NUM_HALL_SENSORS];
        for i in 0..NUM_HALL_SENSORS {
            values[i] = self.read_sensor(i as u8);
        }
        values
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
        self.sr_set_bit(global_bit, true);
        self.sr_write();
        self.sr_set_oe(true);

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
        now_ms() - self.last_pulse_ms[global_bit] >= THERMAL_COOLDOWN_MS
    }

    // ── Shift Registers (SPI) ──────────────────────────────────

    pub fn sr_write(&mut self) {
        let mut buf = [0u8; NUM_SHIFT_REGISTERS];
        for i in 0..NUM_SHIFT_REGISTERS {
            buf[i] = self.sr_state[NUM_SHIFT_REGISTERS - 1 - i];
        }
        self.spi.write(&buf).ok();
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
        if enabled { self.oe.set_low(); } else { self.oe.set_high(); }
    }

    // ── Watchdog ──────────────────────────────────────────────

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
        blocking_delay_ms(50);
        esp_hal::system::software_reset();
        loop {}
    }
}

fn now_ms() -> u64 {
    esp_hal::time::Instant::now().duration_since_epoch().as_millis()
}

pub fn blocking_delay_ms(ms: u32) {
    esp_hal::delay::Delay::new().delay_millis(ms);
}
