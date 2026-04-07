// Manual API extensions for commands that need custom client-side logic.
// These are NOT auto-generated — edit freely.

import { transport } from "./transport";

export interface MultiMoveItem {
  from_x: number;
  from_y: number;
  to_x: number;
  to_y: number;
}

export function moveMulti(moves: MultiMoveItem[]): Promise<{ success: boolean; moves?: boolean[] }> {
  const params: Record<string, number> = {};
  moves.forEach((m, i) => {
    params[`${i}_from_x`] = m.from_x;
    params[`${i}_from_y`] = m.from_y;
    params[`${i}_to_x`] = m.to_x;
    params[`${i}_to_y`] = m.to_y;
  });
  return transport.call("move_multi", params);
}

export function edgeMoveTest(params: {
  direction: string;
  pulse_ms: number;
  duty: number;
  delay_ms: number;
  steps: number;
}): Promise<{ success: boolean }> {
  return transport.call("edge_move_test", params);
}
