use serde::{Deserialize, Serialize};

// ── Grid Constants ────────────────────────────────────────────
pub const GRID_COLS: usize = 10;
pub const GRID_ROWS: usize = 7;
pub const SENSOR_COLS: usize = 4;
pub const SENSOR_ROWS: usize = 3;
pub const MAX_PULSE_MS: u16 = 1000;

// ── Enums ────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[allow(non_camel_case_types)]
pub enum PulseError {
    NONE,
    INVALID_COIL,
    PULSE_TOO_LONG,
    THERMAL_LIMIT,
}

// ── Requests ──────────────────────────────────────────────────

#[derive(Debug, Deserialize)]
pub struct PulseCoilRequest {
    pub x: u8,
    pub y: u8,
    pub duration_ms: u16,
}

#[derive(Debug, Deserialize)]
pub struct SetRGBRequest {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

// ── Responses ─────────────────────────────────────────────────

#[derive(Debug, Serialize)]
pub struct PulseCoilResponse {
    pub success: bool,
    pub error: PulseError,
}

#[derive(Debug, Serialize)]
pub struct GetBoardStateResponse {
    pub raw_strengths: [[u16; SENSOR_ROWS]; SENSOR_COLS],
    pub piece_count: u8,
}

#[derive(Debug, Serialize)]
pub struct SetRGBResponse {
    pub success: bool,
}

#[derive(Debug, Serialize)]
pub struct ShutdownResponse {}
