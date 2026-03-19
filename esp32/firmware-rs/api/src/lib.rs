#![cfg_attr(not(feature = "ts"), no_std)]

use serde::{Deserialize, Serialize};

// ── Derive helpers ────────────────────────────────────────────
// Wrap the verbose cfg_attr + ts-rs boilerplate into single-line macros.

macro_rules! api_response {
    ($(#[$extra:meta])* pub struct $name:ident { $($body:tt)* }) => {
        #[derive(Debug, Serialize)]
        #[cfg_attr(feature = "ts", derive(ts_rs::TS))]
        #[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
        $(#[$extra])*
        pub struct $name { $($body)* }
    };
    ($(#[$extra:meta])* pub enum $name:ident { $($body:tt)* }) => {
        #[derive(Debug, Serialize)]
        #[cfg_attr(feature = "ts", derive(ts_rs::TS))]
        #[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
        $(#[$extra])*
        pub enum $name { $($body)* }
    };
}

macro_rules! api_request {
    ($(#[$extra:meta])* pub struct $name:ident { $($body:tt)* }) => {
        #[derive(Debug, Deserialize)]
        #[cfg_attr(feature = "ts", derive(ts_rs::TS))]
        #[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
        $(#[$extra])*
        pub struct $name { $($body)* }
    };
}

macro_rules! api_enum {
    ($(#[$extra:meta])* pub enum $name:ident { $($body:tt)* }) => {
        #[derive(Debug, Clone, Copy, Serialize, Deserialize)]
        #[cfg_attr(feature = "ts", derive(ts_rs::TS))]
        #[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
        $(#[$extra])*
        pub enum $name { $($body)* }
    };
}

// ── Grid Constants ────────────────────────────────────────────

pub const GRID_COLS: usize = 10;
pub const GRID_ROWS: usize = 7;
pub const SENSOR_COLS: usize = 4;
pub const SENSOR_ROWS: usize = 3;
pub const MAX_PULSE_US: u32 = 500;
pub const NUM_SENSORS: usize = SENSOR_COLS * SENSOR_ROWS;

// ── Enums ────────────────────────────────────────────────────

api_enum! {
    pub enum PulseError {
        InvalidCoil,
        PulseTooLong,
        ThermalLimit,
    }
}

// ── Requests ──────────────────────────────────────────────────

api_request! {
    pub struct PulseCoilParams {
        pub x: u8,
        pub y: u8,
        pub duration_us: u32,
    }
}

api_request! {
    pub struct RGBColor {
        pub r: u8,
        pub g: u8,
        pub b: u8,
    }
}

api_request! {
    pub struct CalibrateParams {
        pub samples: u8,
        pub pulse_us: u32,
    }
}

// ── Responses ─────────────────────────────────────────────────

api_response! {
    #[serde(tag = "status")]
    pub enum PulseResult {
        Success,
        Failure(PulseError),
    }
}

api_response! {
    pub struct BoardState {
        pub raw_strengths: [[u16; SENSOR_ROWS]; SENSOR_COLS],
        pub piece_count: u8,
    }
}

api_response! {
    pub struct SensorCalibration {
        pub baseline: u16,
        pub coil_on: u16,
        pub delta: i32,
    }
}

api_response! {
    pub struct CalibrationResult {
        pub sensors: [SensorCalibration; NUM_SENSORS],
    }
}

api_response! {
    pub struct CommandResult {
        pub success: bool,
    }
}

// ── Events ────────────────────────────────────────────────────

api_response! {
    pub struct BoardChangedEvent {
        pub raw_strengths: [[u16; SENSOR_ROWS]; SENSOR_COLS],
        pub piece_count: u8,
    }
}

api_response! {
    #[serde(tag = "event", content = "data")]
    pub enum BoardEvent {
        #[serde(rename = "board_changed")]
        BoardChanged(BoardChangedEvent),
    }
}
