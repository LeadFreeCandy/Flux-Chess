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

export interface CoilDiag {
  sensor: number;
  min: number;
  detected: boolean;
  arrival: number;
}

export interface MoveDiag {
  checkpoint_ok: boolean;
  retries_used: number;
  coils: CoilDiag[];
}

export interface MoveResponse {
  success: boolean;
  error: any;
  diag?: MoveDiag;
}

export interface MovePhysicsRequest {
  from_x: number;
  from_y: number;
  to_x: number;
  to_y: number;
  max_retries?: number;
}

export interface MovePieceRequest {
  from_x: number;
  from_y: number;
  to_x: number;
  to_y: number;
  max_retries?: number;
}

export interface SetPhysicsParamsRequest {
  piece_mass_g: number;
  max_current_a: number;
  mu_static: number;
  mu_kinetic: number;
  target_velocity_mm_s: number;
  target_accel_mm_s2: number;
  max_jerk_mm_s3: number;
  coast_friction_offset: number;
  brake_pulse_ms: number;
  pwm_freq_hz: number;
  pwm_compensation: number;
  all_coils_equal: boolean;
  force_scale: number;
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
  move_piece: { method: "POST", path: "/api/move_piece" },
  hexapawn_play: { method: "POST", path: "/api/hexapawn/play" },
  set_physics_params: { method: "POST", path: "/api/set_physics_params" },
  get_physics_params: { method: "GET", path: "/api/get_physics_params" },
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

export function movePiece(params: MovePieceRequest): Promise<MoveResponse> {
  return transport.call("move_piece", params);
}

export function hexapawnPlay(): Promise<GetCalibrationResponse> {
  return transport.call("hexapawn_play", {});
}

export function setPhysicsParams(params: SetPhysicsParamsRequest): Promise<ShutdownResponse> {
  return transport.call("set_physics_params", params);
}

export function getPhysicsParams(): Promise<GetCalibrationResponse> {
  return transport.call("get_physics_params", {});
}

export function tunePhysics(): Promise<GetCalibrationResponse> {
  return transport.call("tune_physics", {});
}

export function calibrate(): Promise<CalibrateResponse> {
  return transport.call("calibrate", {});
}

export function getCalibration(): Promise<GetCalibrationResponse> {
  return transport.call("get_calibration", {});
}
