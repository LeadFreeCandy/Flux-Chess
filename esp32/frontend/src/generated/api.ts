// AUTO-GENERATED from firmware/api.h — do not edit
// Run: python codegen/generate.py

import { transport } from "../transport";

export interface Position {
  x: number;
  y: number;
}

export interface PiecePosition {
  piece_id: number;
  pos: Position;
}

export type ShutdownRequest = Record<string, never>;

export interface PulseCoilRequest {
  x: number;
  y: number;
  duration_ms: number;
}

export type GetBoardStateRequest = Record<string, never>;

export interface SetRGBRequest {
  r: number;
  g: number;
  b: number;
}

export type ShutdownResponse = Record<string, never>;

export interface PulseCoilResponse {
  success: boolean;
  error: string;
}

export interface GetBoardStateResponse {
  raw_strengths: number[][];
  pieces: number[][];
}

export interface SetRGBResponse {
  success: boolean;
}

export const commands = {
  shutdown: { method: "POST", path: "/api/shutdown" },
  pulse_coil: { method: "POST", path: "/api/pulse_coil" },
  get_board_state: { method: "GET", path: "/api/board_state" },
  set_rgb: { method: "POST", path: "/api/set_rgb" },
} as const;

export function shutdown(): Promise<ShutdownResponse> {
  return transport.call("shutdown", {});
}

export function pulseCoil(params: PulseCoilRequest): Promise<PulseCoilResponse> {
  return transport.call("pulse_coil", params);
}

export function getBoardState(): Promise<GetBoardStateResponse> {
  return transport.call("get_board_state", {});
}

export function setRgb(params: SetRGBRequest): Promise<SetRGBResponse> {
  return transport.call("set_rgb", params);
}
