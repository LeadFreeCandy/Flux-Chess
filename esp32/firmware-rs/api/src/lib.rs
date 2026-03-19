#![cfg_attr(not(feature = "ts"), no_std)]

use serde::{Deserialize, Serialize};

// ── Grid Constants ────────────────────────────────────────────

pub const GRID_COLS: usize = 10;
pub const GRID_ROWS: usize = 7;
pub const SENSOR_COLS: usize = 4;
pub const SENSOR_ROWS: usize = 3;
pub const MAX_PULSE_US: u32 = 500;

// ── Enums ────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
#[allow(non_camel_case_types)]
pub enum PulseError {
    InvalidCoil,
    PulseTooLong,
    ThermalLimit,
}

// ── Types ─────────────────────────────────────────────────────

#[derive(Debug, Deserialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct PulseCoilParams {
    pub x: u8,
    pub y: u8,
    pub duration_us: u32,
}

#[derive(Debug, Deserialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct RGBColor {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

#[derive(Debug, Serialize)]
#[serde(tag = "status")]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub enum PulseResult {
    Success,
    Failure(PulseError),
}

#[derive(Debug, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct BoardState {
    pub raw_strengths: [[u16; SENSOR_ROWS]; SENSOR_COLS],
    pub piece_count: u8,
}

pub const NUM_SENSORS: usize = SENSOR_COLS * SENSOR_ROWS;

#[derive(Debug, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct SensorCalibration {
    pub baseline: u16,
    pub coil_on: u16,
    pub delta: i32,
}

#[derive(Debug, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct CalibrationResult {
    pub sensors: [SensorCalibration; NUM_SENSORS],
}

#[derive(Debug, Deserialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct CalibrateParams {
    pub samples: u8,
    pub pulse_us: u32,
}

#[derive(Debug, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(
    feature = "ts",
    ts(export, export_to = "../../../frontend/src/generated/bindings/")
)]
pub struct CommandResult {
    pub success: bool,
}

// ── Events (server → client, unsolicited) ─────────────────────

#[derive(Debug, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
pub struct BoardChangedEvent {
    pub raw_strengths: [[u16; SENSOR_ROWS]; SENSOR_COLS],
    pub piece_count: u8,
}

#[derive(Debug, Serialize)]
#[serde(tag = "event", content = "data")]
#[cfg_attr(feature = "ts", derive(ts_rs::TS))]
#[cfg_attr(feature = "ts", ts(export, export_to = "../../../frontend/src/generated/bindings/"))]
pub enum BoardEvent {
    #[serde(rename = "board_changed")]
    BoardChanged(BoardChangedEvent),
}
