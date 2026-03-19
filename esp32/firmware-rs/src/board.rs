extern crate alloc;
use alloc::vec::Vec;

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
    event_queue: Vec<BoardEvent>,
}

impl Board {
    pub fn new(hw: Hardware) -> Self {
        Self {
            hw,
            event_queue: Vec::new(),
        }
    }

    // ── Monitor Tick ──────────────────────────────────────────
    // Called from the main loop. Reads sensors and pushes events.

    pub fn tick(&mut self) {
        let state = self.get_board_state();
        self.event_queue.push(BoardEvent::BoardChanged);
    }

    pub fn drain_events(&mut self) -> Vec<BoardEvent> {
        core::mem::take(&mut self.event_queue)
    }

    // ── Coil Control ──────────────────────────────────────────

    pub fn pulse_coil(&mut self, x: u8, y: u8, duration_us: u32) -> PulseResult {
        log::info!(
            "pulseCoil: request at grid ({},{}) for {}us",
            x,
            y,
            duration_us
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
                log::warn!(
                    "pulseCoil REJECT: ({},{}) no coil (local {},{} in block {},{})",
                    x,
                    y,
                    lx,
                    ly,
                    x as usize / SR_BLOCK,
                    y as usize / SR_BLOCK
                );
                return PulseResult::Failure(PulseError::InvalidCoil);
            }
        };

        let sr = bit / 8;
        let pin = bit % 8;
        log::info!(
            "pulseCoil: ({},{}) -> SR{} pin {} (bit {}), delegating to hw",
            x,
            y,
            sr,
            pin,
            bit
        );

        let result = self.hw.pulse_bit(bit, duration_us);
        match &result {
            PulseResult::Success => log::info!(
                "pulseCoil OK: ({},{}) pulsed for {}us via SR{} pin {}",
                x,
                y,
                duration_us,
                sr,
                pin
            ),
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

    pub fn calibrate(&mut self, samples: u8, pulse_us: u32) -> CalibrationResult {
        let samples = if samples == 0 { 10 } else { samples };
        let pulse_us = if pulse_us == 0 { 50 } else { pulse_us };
        log::info!("calibrate: {} samples, {}us pulse", samples, pulse_us);

        let mut result = CalibrationResult {
            sensors: core::array::from_fn(|_| core::array::from_fn(|_| SensorCalibration {
                baseline: 0, coil_on: 0, magnet_present: 0,
            })),
        };

        // Step 1: measure baseline (all coils off, average over N samples)
        log::info!("calibrate: measuring baseline...");
        let mut sums = [[0u32; SENSOR_ROWS]; SENSOR_COLS];
        for _ in 0..samples {
            let raw = self.hw.read_all_sensors();
            for i in 0..NUM_HALL_SENSORS {
                let col = i % SENSOR_COLS;
                let row = i / SENSOR_COLS;
                sums[col][row] += raw[i] as u32;
            }
            crate::hardware::blocking_delay_us(10_000);
        }
        for col in 0..SENSOR_COLS {
            for row in 0..SENSOR_ROWS {
                result.sensors[col][row].baseline = (sums[col][row] / samples as u32) as u16;
            }
        }

        // Step 2: for each sensor, pulse its aligned coil and read
        log::info!("calibrate: measuring coil-on levels...");
        for col in 0..SENSOR_COLS {
            for row in 0..SENSOR_ROWS {
                let i = col + row * SENSOR_COLS;
                let x = (col * SR_BLOCK) as u8;
                let y = (row * SR_BLOCK) as u8;

                let bit = match Self::coord_to_bit(x, y) {
                    Some(b) => b,
                    None => {
                        log::warn!("calibrate: no coil at ({},{}) for sensor {}", x, y, i);
                        continue;
                    }
                };

                self.hw.sr_set_bit(bit, true);
                self.hw.sr_write();
                self.hw.sr_set_oe(true);
                crate::hardware::blocking_delay_us(pulse_us);

                let reading = self.hw.read_sensor(i as u8);
                result.sensors[col][row].coil_on = reading;

                self.hw.sr_set_bit(bit, false);
                self.hw.sr_write();
                self.hw.sr_set_oe(false);

                log::info!(
                    "calibrate: sensor ({},{}) baseline={} coil_on={}",
                    col, row,
                    result.sensors[col][row].baseline,
                    result.sensors[col][row].coil_on,
                );

                crate::hardware::blocking_delay_us(100_000);
            }
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
        self.hw.watchdog_tick(MAX_PULSE_US);
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
