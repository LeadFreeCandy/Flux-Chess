use crate::api::*;
use crate::hardware::Hardware;
use crate::pins::NUM_HALL_SENSORS;

// ── Coil Grid Mapping ─────────────────────────────────────
//
// 12 shift registers in a 4-column × 3-row grid.
// Each SR drives 5 coils in an L-shape within a 3×3 block:
//
//   (0,2)  .     .       bit 4
//   (0,1)  .     .       bit 3
//   (0,0) (1,0) (2,0)    bit 2, bit 1, bit 0
//
// SR layout (4 cols × 3 rows, each offset by 3):
//   SR0  @ (0,0)   SR1  @ (3,0)   SR2  @ (6,0)   SR3  @ (9,0)
//   SR4  @ (0,3)   SR5  @ (3,3)   SR6  @ (6,3)   SR7  @ (9,3)
//   SR8  @ (0,6)   SR9  @ (3,6)   SR10 @ (6,6)   SR11 @ (9,6)

const SR_COLS: usize = 4;
const SR_ROWS: usize = 3;
const SR_BLOCK: usize = 3;

pub struct Board {
    hw: Hardware,
}

impl Board {
    pub fn new(hw: Hardware) -> Self {
        Self { hw }
    }

    // ── Coil Control ──────────────────────────────────────────

    pub fn pulse_coil(&mut self, x: u8, y: u8, duration_ms: u16) -> PulseResult {
        log::info!(
            "pulseCoil: request at grid ({},{}) for {}ms",
            x,
            y,
            duration_ms
        );

        if x as usize >= GRID_COLS || y as usize >= GRID_ROWS {
            log::warn!(
                "pulseCoil REJECT: ({},{}) out of bounds ({}x{})",
                x,
                y,
                GRID_COLS,
                GRID_ROWS
            );
            return PulseResult::Failure(PulseError::InvalidCoil);
        }

        let bit = match Self::coord_to_bit(x, y) {
            Some(b) => b,
            None => {
                let lx = x as usize % SR_BLOCK;
                let ly = y as usize % SR_BLOCK;
                log::warn!("pulseCoil REJECT: ({},{}) no coil (local {},{} in block {},{})",
                    x, y, lx, ly, x as usize / SR_BLOCK, y as usize / SR_BLOCK);
                return PulseResult::Failure(PulseError::InvalidCoil);
            }
        };

        let sr = bit / 8;
        let pin = bit % 8;
        log::info!("pulseCoil: ({},{}) -> SR{} pin {} (bit {}), delegating to hw", x, y, sr, pin, bit);

        let result = self.hw.pulse_bit(bit, duration_ms);
        match &result {
            PulseResult::Success => log::info!("pulseCoil OK: ({},{}) pulsed for {}ms via SR{} pin {}", x, y, duration_ms, sr, pin),
            PulseResult::Failure(e) => log::warn!("pulseCoil FAIL: SR{} pin {} ({:?})", sr, pin, e),
        }
        result
    }

    // ── Board State ───────────────────────────────────────────

    pub fn get_board_state(&mut self) -> BoardState {
        let raw = self.hw.read_all_sensors();
        let mut strengths = [[0u16; SENSOR_ROWS]; SENSOR_COLS];
        for i in 0..NUM_HALL_SENSORS {
            let col = i % SENSOR_COLS;
            let row = i / SENSOR_COLS;
            strengths[col][row] = raw[i];
        }
        BoardState {
            raw_strengths: strengths,
            piece_count: 0,
        }
    }

    // ── Calibration ────────────────────────────────────────────
    //
    // Each sensor is aligned with one coil at (col*3, row*3):
    //   sensor 0 → (0,0), sensor 1 → (3,0), sensor 2 → (6,0), sensor 3 → (9,0)
    //   sensor 4 → (0,3), sensor 5 → (3,3), sensor 6 → (6,3), sensor 7 → (9,3)
    //   sensor 8 → (0,6), sensor 9 → (3,6), sensor 10 → (6,6), sensor 11 → (9,6)

    pub fn calibrate(&mut self, samples: u8, pulse_ms: u16) -> CalibrationResult {
        let samples = if samples == 0 { 10 } else { samples };
        let pulse_ms = if pulse_ms == 0 { 50 } else { pulse_ms };
        log::info!("calibrate: {} samples, {}ms pulse", samples, pulse_ms);

        let mut result = CalibrationResult {
            sensors: core::array::from_fn(|_| SensorCalibration {
                baseline: 0,
                coil_on: 0,
                delta: 0,
            }),
        };

        // Step 1: measure baseline (all coils off, average over N samples)
        log::info!("calibrate: measuring baseline...");
        let mut sums = [0u32; NUM_SENSORS];
        for _ in 0..samples {
            let raw = self.hw.read_all_sensors();
            for i in 0..NUM_SENSORS {
                sums[i] += raw[i] as u32;
            }
            crate::hardware::blocking_delay_ms(10);
        }
        for i in 0..NUM_SENSORS {
            result.sensors[i].baseline = (sums[i] / samples as u32) as u16;
        }

        // Step 2: for each sensor, pulse its aligned coil and read
        log::info!("calibrate: measuring coil-on levels...");
        for i in 0..NUM_SENSORS {
            let col = (i % SENSOR_COLS) as u8;
            let row = (i / SENSOR_COLS) as u8;
            let x = col * SR_BLOCK as u8;
            let y = row * SR_BLOCK as u8;

            let bit = match Self::coord_to_bit(x, y) {
                Some(b) => b,
                None => {
                    log::warn!("calibrate: no coil at ({},{}) for sensor {}", x, y, i);
                    continue;
                }
            };

            // Pulse the coil
            self.hw.sr_set_bit(bit, true);
            self.hw.sr_write();
            self.hw.sr_set_oe(true);
            crate::hardware::blocking_delay_ms(pulse_ms as u32);

            // Read sensor while coil is on
            let reading = self.hw.read_sensor(i as u8);
            result.sensors[i].coil_on = reading;

            // Turn off
            self.hw.sr_set_bit(bit, false);
            self.hw.sr_write();
            self.hw.sr_set_oe(false);

            result.sensors[i].delta = reading as i32 - result.sensors[i].baseline as i32;

            log::info!(
                "calibrate: sensor {} ({},{}) baseline={} coil_on={} delta={}",
                i,
                x,
                y,
                result.sensors[i].baseline,
                result.sensors[i].coil_on,
                result.sensors[i].delta
            );

            // Wait for thermal cooldown between coils
            crate::hardware::blocking_delay_ms(100);
        }

        log::info!("calibrate: done");
        result
    }

    // ── RGB ───────────────────────────────────────────────────

    pub fn set_rgb(&mut self, r: u8, g: u8, b: u8) -> CommandResult {
        log::info!(
            "setRGB: r={} g={} b={} (#{:02X}{:02X}{:02X})",
            r,
            g,
            b,
            r,
            g,
            b
        );
        self.hw.set_rgb(r, g, b);
        CommandResult { success: true }
    }

    // ── System ────────────────────────────────────────────────

    pub fn shutdown(&mut self) -> ! {
        log::info!("shutdown: delegating to hardware for safe powerdown");
        self.hw.shutdown()
    }

    // ── Watchdog passthrough ──────────────────────────────────

    pub fn watchdog_tick(&mut self) {
        self.hw.watchdog_tick(MAX_PULSE_MS);
    }

    // ── Grid Mapping ──────────────────────────────────────────

    fn coord_to_bit(x: u8, y: u8) -> Option<usize> {
        let sr_col = x as usize / SR_BLOCK;
        let sr_row = y as usize / SR_BLOCK;
        if sr_col >= SR_COLS || sr_row >= SR_ROWS {
            return None;
        }

        let sr_index = sr_row * SR_COLS + sr_col;
        let lx = x as usize % SR_BLOCK;
        let ly = y as usize % SR_BLOCK;

        let local_bit = if ly == 0 {
            // Bottom row: bit 2 at lx=0, bit 1 at lx=1, bit 0 at lx=2
            Some(2 - lx)
        } else if lx == 0 {
            // Left column going up: bit 3 at ly=1, bit 4 at ly=2
            Some(2 + ly)
        } else {
            None
        };

        local_bit.map(|b| sr_index * 8 + b)
    }
}
