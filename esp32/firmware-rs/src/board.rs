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

pub struct Board<'d> {
    hw: Hardware<'d>,
}

impl<'d> Board<'d> {
    pub fn new(hw: Hardware<'d>) -> Self {
        Self { hw }
    }

    // ── Coil Control ──────────────────────────────────────────

    pub fn pulse_coil(&mut self, x: u8, y: u8, duration_ms: u16) -> PulseResult {
        log::info!("pulseCoil: request at grid ({},{}) for {}ms", x, y, duration_ms);

        if x as usize >= GRID_COLS || y as usize >= GRID_ROWS {
            log::warn!("pulseCoil REJECT: ({},{}) out of bounds ({}x{})", x, y, GRID_COLS, GRID_ROWS);
            return PulseResult { success: false, error: PulseError::INVALID_COIL };
        }

        if duration_ms > MAX_PULSE_MS {
            log::warn!("pulseCoil REJECT: {}ms exceeds max {}ms", duration_ms, MAX_PULSE_MS);
            return PulseResult { success: false, error: PulseError::PULSE_TOO_LONG };
        }

        let bit = match Self::coord_to_bit(x, y) {
            Some(b) => b,
            None => {
                let lx = x as usize % SR_BLOCK;
                let ly = y as usize % SR_BLOCK;
                log::warn!("pulseCoil REJECT: ({},{}) no coil (local {},{} in block {},{})",
                    x, y, lx, ly, x as usize / SR_BLOCK, y as usize / SR_BLOCK);
                return PulseResult { success: false, error: PulseError::INVALID_COIL };
            }
        };

        let sr = bit / 8;
        let pin = bit % 8;
        log::info!("pulseCoil: ({},{}) -> SR{} pin {} (bit {}), delegating to hw", x, y, sr, pin, bit);

        if !self.hw.pulse_bit(bit, duration_ms) {
            log::warn!("pulseCoil FAIL: hw refused SR{} pin {} (thermal)", sr, pin);
            return PulseResult { success: false, error: PulseError::THERMAL_LIMIT };
        }

        log::info!("pulseCoil OK: ({},{}) pulsed for {}ms via SR{} pin {}", x, y, duration_ms, sr, pin);
        PulseResult { success: true, error: PulseError::NONE }
    }

    // ── Board State ───────────────────────────────────────────

    pub fn get_board_state(&self) -> BoardState {
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

    // ── RGB ───────────────────────────────────────────────────

    pub fn set_rgb(&mut self, r: u8, g: u8, b: u8) -> CommandResult {
        log::info!("setRGB: r={} g={} b={} (#{:02X}{:02X}{:02X})", r, g, b, r, g, b);
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
        if sr_col >= SR_COLS || sr_row >= SR_ROWS { return None; }

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
