// Matches Rust firmware API (firmware-rs/api/src/lib.rs)
// Types auto-generated via ts-rs in firmware-rs/frontend/src/generated/bindings/

import { transport } from "../transport";

// ── Types ─────────────────────────────────────────────────────

export type PulseError = "InvalidCoil" | "PulseTooLong" | "ThermalLimit";

export type PulseResult =
  | { status: "Success" }
  | { status: "Failure"; error: PulseError };

export interface BoardStateResponse {
  raw_sensor_values: number[][];
  ids: number[][];
  timestamps: number[][];
}

export interface PulseCoilParams {
  x: number;
  y: number;
  duration_us: number;
}

export interface RGBColor {
  r: number;
  g: number;
  b: number;
}

export interface CommandResult {
  success: boolean;
}

// ── Command registry (used by HTTP transport) ─────────────────

export const commands = {
  get_board_state: { method: "GET", path: "/api/board_state" },
  pulse_coil: { method: "POST", path: "/api/pulse_coil" },
  set_rgb: { method: "POST", path: "/api/set_rgb" },
  calibrate: { method: "POST", path: "/api/calibrate" },
  shutdown: { method: "POST", path: "/api/shutdown" },
} as const;

// ── API functions ─────────────────────────────────────────────

export function getBoardState(): Promise<BoardStateResponse> {
  return transport.call("get_board_state", {});
}

export function pulseCoil(params: PulseCoilParams): Promise<PulseResult> {
  return transport.call("pulse_coil", params);
}

export function setRgb(params: RGBColor): Promise<CommandResult> {
  return transport.call("set_rgb", params);
}

export function shutdown(): Promise<CommandResult> {
  return transport.call("shutdown", {});
}
