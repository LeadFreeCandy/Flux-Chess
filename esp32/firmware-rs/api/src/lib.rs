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

macro_rules! api_data {
    ($(#[$extra:meta])* pub struct $name:ident { $($body:tt)* }) => {
        #[derive(Debug, Serialize, Deserialize)]
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
pub const MAIN_GRID_COLS: usize = 10;
pub const MAIN_GRID_ROWS: usize = 7;

pub const MAX_PULSE_US: u32 = 500;
pub const NUM_SENSORS: usize = SENSOR_COLS * SENSOR_ROWS;

pub type SensorGrid<T> = [[T; SENSOR_ROWS]; SENSOR_COLS];
pub type CoilGrid<T> = [[T; GRID_ROWS]; GRID_COLS];
pub type MainGrid<T> = [[T; MAIN_GRID_ROWS]; MAIN_GRID_COLS];

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
    #[serde(tag = "status", content = "error")]
    pub enum PulseResult {
        Success,
        Failure(PulseError),
    }
}

api_response! {
    pub struct BoardState {
        pub raw_sensor_values: SensorGrid<u16>,
        pub ids: CoilGrid<u8>,
        pub timestamps: CoilGrid<u64>,
    }
}

impl PartialEq for BoardState {
    fn eq(&self, other: &Self) -> bool {
        self.ids == other.ids
    }
}

impl Eq for BoardState {}

impl BoardState {
    pub fn update_from_sensor_values(
        &mut self,
        raw_values: &SensorGrid<u16>,
        calibration: &CalibrationResult,
        debounce_duration_ms: u32,
    ) {
        // We need to compare the new sensor readings to the the old ones using the
        // calibration values to determine if a piece has moved. Then we will update the
        // current board ids
        for row in 0..SENSOR_ROWS {
            for col in 0..SENSOR_COLS {
                let new_sensor_value = raw_values[col][row];
                let sensor_calibration = &calibration.sensors[col][row];
                let old_sensor_value = self.raw_sensor_values[col][row];
            }
        }
    }
}

api_data! {
    pub struct SensorCalibration {
        pub baseline: u16,
        pub coil_on: u16,
        pub magnet_present: u16,
    }
}

api_data! {
    pub struct CalibrationResult {
        pub sensors: SensorGrid<SensorCalibration>,
    }
}

api_response! {
    pub struct CommandResult {
        pub success: bool,
    }
}

// ── Events ────────────────────────────────────────────────────

api_response! {
    #[serde(tag = "event", content = "data")]
    pub enum BoardEvent {
        BoardChanged
    }
}
