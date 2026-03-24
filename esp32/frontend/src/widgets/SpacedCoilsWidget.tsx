import { useState } from "react";
import { pulseCoil } from "../generated/api";
import { type WidgetProps, labelStyle, inputStyle } from "./shared";

const GRID_COLS = 10, GRID_ROWS = 7, SR_BLOCK = 3, SR_COLS = 4, SR_ROWS = 3;

function hasCoil(x: number, y: number): boolean {
  if (x >= GRID_COLS || y >= GRID_ROWS) return false;
  const srCol = Math.floor(x / SR_BLOCK), srRow = Math.floor(y / SR_BLOCK);
  if (srCol >= SR_COLS || srRow >= SR_ROWS) return false;
  const lx = x % SR_BLOCK, ly = y % SR_BLOCK;
  return ly === 0 || (lx === 0 && ly > 0);
}

export default function SpacedCoilsWidget({ onStatus }: WidgetProps) {
  const [pulseDuration, setPulseDuration] = useState(100);
  const [pulsing, setPulsing] = useState<string | null>(null);

  const handleClick = async (x: number, y: number) => {
    const key = `${x},${y}`;
    setPulsing(key);
    try {
      const res = await pulseCoil({ x, y, duration_ms: pulseDuration });
      onStatus(res.success ? `Pulsed (${x},${y})` : `Error: ${res.error}`);
    } catch (e) { onStatus(`Error: ${e}`); }
    setPulsing(null);
  };

  return (
    <>
      <div style={{ marginBottom: 12 }}>
        <label style={labelStyle}>
          Pulse duration
          <input type="number" value={pulseDuration} min={1} max={1000} style={{ ...inputStyle, width: 70 }} onChange={(e) => setPulseDuration(Number(e.target.value))} /> ms
        </label>
      </div>
      <div style={{ display: "grid", gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`, gap: 3, maxWidth: 520, margin: "0 auto" }}>
        {Array.from({ length: GRID_ROWS }, (_, y) =>
          Array.from({ length: GRID_COLS }, (_, x) => {
            const ry = GRID_ROWS - 1 - y, coil = hasCoil(x, ry);
            const key = `${x},${ry}`, isPulsing = pulsing === key;
            return (
              <button
                key={key} disabled={!coil} onClick={() => coil && handleClick(x, ry)}
                style={{
                  width: "100%", height: 30, borderRadius: 4, fontSize: 9, cursor: coil ? "pointer" : "default", fontFamily: "inherit", padding: 0,
                  border: coil ? "1px solid #3a5a7c" : "1px solid transparent",
                  background: isPulsing ? "#f57f17" : coil ? "#1a3a5c" : "transparent",
                  color: coil ? "#7ab8e0" : "transparent",
                }}
              >{coil ? `${x},${ry}` : ""}</button>
            );
          })
        )}
      </div>
    </>
  );
}