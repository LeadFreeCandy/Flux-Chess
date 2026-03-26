import { useState } from "react";
import { moveDumb, movePhysics, getBoardState } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULT_PARAMS = {
  piece_mass_g: 4.3,
  max_current_a: 1.0,
  mu_static: 0.35,
  mu_kinetic: 0.25,
  target_velocity_mm_s: 100,
  target_accel_mm_s2: 500,
  max_duration_ms: 5000,
};

const PARAM_LABELS: Record<keyof typeof DEFAULT_PARAMS, string> = {
  piece_mass_g:         "mass (g)",
  max_current_a:        "max current (A)",
  mu_static:            "static mu",
  mu_kinetic:           "kinetic mu",
  target_velocity_mm_s: "target v (mm/s)",
  target_accel_mm_s2:   "target a (mm/s\u00B2)",
  max_duration_ms:      "timeout ms",
};

export default function MoveTestWidget({ onStatus }: WidgetProps) {
  const [params, setParams] = useState(DEFAULT_PARAMS);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const updateParam = (key: keyof typeof DEFAULT_PARAMS, val: string) => {
    setParams(p => ({ ...p, [key]: parseFloat(val) || 0 }));
  };

  const runTest = async () => {
    setRunning(true);
    setResult(null);
    const collected: string[] = [];
    setLogs([]);
    onStatus("Running move test...");

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("physics:")) {
            collected.push(msg.msg);
            setLogs([...collected]);

            // Parse position from log: "physics: t=XXms pos=(X.XX,Y.YY) ..."
            const posMatch = msg.msg.match(/pos=\((-?[\d.]+),(-?[\d.]+)\)/);
            if (posMatch) {
              window.dispatchEvent(new CustomEvent("fluxchess-sim-pos", {
                detail: { x: parseFloat(posMatch[1]), y: parseFloat(posMatch[2]) }
              }));
            }
          }
        } catch {}
      }
    });

    try {
      // Phase 1: physics move (0,3) -> (9,3)
      onStatus("Physics move (0,3) -> (9,3)...");
      const moveRes = await movePhysics({
        from_x: 0, from_y: 3, to_x: 9, to_y: 3,
        ...params,
      });

      const moveOk = moveRes.success;
      collected.push(`--- RESULT: ${moveOk ? "SUCCESS" : "FAILED"} ---`);
      setLogs([...collected]);

      // Read sensors to see where piece ended up
      const state = await getBoardState();
      const strengths = state.raw_strengths;
      const row1 = strengths.map((col: number[]) => col[1]); // row index 1 = y=3
      collected.push(`Sensor row y=3: [${row1.join(", ")}]`);
      setLogs([...collected]);

      // Phase 2: sweep back using dumb moves (9,3) -> (0,3)
      onStatus("Sweeping back (9,3) -> (0,3)...");
      // Chain of dumb moves to sweep piece back regardless of where it is
      for (let x = 9; x > 0; x--) {
        await moveDumb({ from_x: x, from_y: 3, to_x: x - 1, to_y: 3 });
      }

      collected.push("--- SWEEP BACK COMPLETE ---");
      setLogs([...collected]);

      // Read final state
      const finalState = await getBoardState();
      const finalRow = finalState.raw_strengths.map((col: number[]) => col[1]);
      collected.push(`Final sensor row y=3: [${finalRow.join(", ")}]`);
      setLogs([...collected]);

      setResult(moveOk ? "Move succeeded" : "Move failed");
      onStatus(moveOk ? "Test complete - success" : "Test complete - failed");
    } catch (e) {
      collected.push(`ERROR: ${e}`);
      setLogs([...collected]);
      setResult(`Error: ${e}`);
      onStatus(`Test error: ${e}`);
    }

    unsub();
    window.dispatchEvent(new CustomEvent("fluxchess-sim-pos", { detail: null }));
    setRunning(false);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%" }}>
      <div style={{ fontSize: 12, color: "#888" }}>
        Place piece at (0,3). Runs physics move to (9,3), then sweeps back.
      </div>

      {/* Params grid */}
      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "4px 8px",
        background: "#151525", padding: 8, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        {(Object.keys(DEFAULT_PARAMS) as (keyof typeof DEFAULT_PARAMS)[]).map(key => (
          <label key={key} style={{ ...labelStyle, fontSize: 10 }}>
            {PARAM_LABELS[key]}
            <input
              type="number"
              step={key === "max_duration_ms" ? 1 : (key === "mu_static" || key === "mu_kinetic") ? 0.01 : (key === "target_velocity_mm_s" || key === "target_accel_mm_s2") ? 1 : 0.1}
              value={params[key]}
              onChange={e => updateParam(key, e.target.value)}
              style={{ ...inputStyle, width: 55, fontSize: 10 }}
              disabled={running}
            />
          </label>
        ))}
      </div>

      <button onClick={runTest} disabled={running} style={{ ...btnStyle, width: "100%", opacity: running ? 0.5 : 1 }}>
        {running ? "Running..." : "Run Move Test"}
      </button>

      {result && (
        <div style={{
          padding: 8, borderRadius: 4, fontSize: 13, fontWeight: "bold", textAlign: "center",
          background: result.includes("success") ? "#1b5e20" : "#b71c1c",
          color: "#fff",
        }}>
          {result}
        </div>
      )}

      {logs.length > 0 && (
        <div style={{
          flex: 1, minHeight: 150, maxHeight: 400, overflow: "auto",
          background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6,
          padding: 8, fontSize: 10, fontFamily: "monospace", color: "#8f8",
          userSelect: "text", cursor: "text",
        }}>
          {logs.map((line, i) => (
            <div key={i} style={{ color: line.startsWith("---") ? "#4fc3f7" : line.startsWith("Sensor") || line.startsWith("Final") ? "#ff9800" : "#8f8" }}>
              {line}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
