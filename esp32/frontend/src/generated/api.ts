// Synced with C++ firmware api.h
import { transport } from "../transport";

// ── Piece Constants ───────────────────────────────────────────
export const PIECE_NONE = 0;
export const PIECE_WHITE = 1;
export const PIECE_BLACK = 2;

// ── Types ─────────────────────────────────────────────────────

export interface PulseCoilRequest {
  x: number;
  y: number;
  duration_ms: number;
}

export interface PulseCoilResponse {
  success: boolean;
  error: string;
}

export interface GetBoardStateResponse {
  raw_strengths: number[][];
  pieces: number[][];
}

export type MoveError = "NONE" | "OUT_OF_BOUNDS" | "SAME_POSITION" | "NOT_ORTHOGONAL" | "NO_PIECE_AT_SOURCE" | "PATH_BLOCKED" | "COIL_FAILURE";

export interface MoveResult {
  success: boolean;
  error: MoveError;
}

export interface SetRGBResponse {
  success: boolean;
}

export interface CommandResult {
  success: boolean;
}

// ── Command Registry ──────────────────────────────────────────

export const commands = {
  shutdown: { method: "POST", path: "/api/shutdown" },
  pulse_coil: { method: "POST", path: "/api/pulse_coil" },
  get_board_state: { method: "GET", path: "/api/board_state" },
  set_rgb: { method: "POST", path: "/api/set_rgb" },
  set_piece: { method: "POST", path: "/api/set_piece" },
  move_dumb: { method: "POST", path: "/api/move_dumb" },
} as const;

// ── API Functions ─────────────────────────────────────────────

export function shutdown(): Promise<CommandResult> {
  return transport.call("shutdown", {});
}

export function pulseCoil(params: PulseCoilRequest): Promise<PulseCoilResponse> {
  return transport.call("pulse_coil", params);
}

export function getBoardState(): Promise<GetBoardStateResponse> {
  return transport.call("get_board_state", {});
}

export function setRgb(params: { r: number; g: number; b: number }): Promise<SetRGBResponse> {
  return transport.call("set_rgb", params);
}

export function setPiece(params: { x: number; y: number; id: number }): Promise<CommandResult> {
  return transport.call("set_piece", params);
}

export function moveDumb(params: { from_x: number; from_y: number; to_x: number; to_y: number }): Promise<MoveResult> {
  return transport.call("move_dumb", params);
}

