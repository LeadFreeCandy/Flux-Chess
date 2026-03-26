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
  error: any;
}

export interface GetBoardStateResponse {
  raw_strengths: number[][];
  pieces: number[][];
}

export interface SetRGBResponse {
  success: boolean;
}

export interface SetPieceRequest {
  x: number;
  y: number;
  id: number;
}

export type CalibrateRequest = Record<string, never>;

export interface CalibrateResponse {
  success: boolean;
}

export type GetCalibrationRequest = Record<string, never>;

export interface GetCalibrationResponse {
  data: string;
}

export interface MoveDumbRequest {
  from_x: number;
  from_y: number;
  to_x: number;
  to_y: number;
}

export interface MoveResponse {
  success: boolean;
  error: any;
}

export interface MovePhysicsRequest {
  from_x: number;
  from_y: number;
  to_x: number;
  to_y: number;
  force_k: number;
  force_epsilon: number;
  falloff_exp: number;
  voltage_scale: number;
  friction_static: number;
  friction_kinetic: number;
  target_velocity: number;
  target_accel: number;
  sensor_k: number;
  sensor_falloff: number;
  sensor_threshold: number;
  manual_baseline: number;
  manual_piece_mean: number;
  max_duration_ms: number;
}

export const commands = {
  shutdown: { method: "POST", path: "/api/shutdown" },
  pulse_coil: { method: "POST", path: "/api/pulse_coil" },
  get_board_state: { method: "GET", path: "/api/board_state" },
  set_rgb: { method: "POST", path: "/api/set_rgb" },
  set_piece: { method: "POST", path: "/api/set_piece" },
  move_dumb: { method: "POST", path: "/api/move_dumb" },
  move_physics: { method: "POST", path: "/api/move_physics" },
  tune_physics: { method: "POST", path: "/api/tune_physics" },
  calibrate: { method: "POST", path: "/api/calibrate" },
  get_calibration: { method: "GET", path: "/api/calibration" },
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

export function setPiece(params: SetPieceRequest): Promise<SetRGBResponse> {
  return transport.call("set_piece", params);
}

export function moveDumb(params: MoveDumbRequest): Promise<MoveResponse> {
  return transport.call("move_dumb", params);
}

export function movePhysics(params: MovePhysicsRequest): Promise<MoveResponse> {
  return transport.call("move_physics", params);
}

export function tunePhysics(params: MovePhysicsRequest): Promise<GetCalibrationResponse> {
  return transport.call("tune_physics", params);
}

export function calibrate(): Promise<CalibrateResponse> {
  return transport.call("calibrate", {});
}

export function getCalibration(): Promise<GetCalibrationResponse> {
  return transport.call("get_calibration", {});
}
