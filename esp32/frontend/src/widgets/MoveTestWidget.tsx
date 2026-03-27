import { useState } from "react";
import { moveDumb, movePhysics, getBoardState, setPiece } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULT_PARAMS = {
  piece_mass_g: 4.3,
  max_current_a: 1.0,
  mu_static: 0.35,
  mu_kinetic: 0.25,
  target_velocity_mm_s: 100,
  target_accel_mm_s2: 500,
  max_jerk_mm_s3: 50000,
  active_brake: true,
  pwm_freq_hz: 20000,
  pwm_compensation: 0.2,
  all_coils_equal: false,
  force_scale: 1.0,
  max_duration_ms: 5000,
};

const PARAM_LABELS: Record<string, string> = {
  piece_mass_g:         "mass (g)",
  max_current_a:        "max current (A)",
  mu_static:            "static mu",
  mu_kinetic:           "kinetic mu",
  target_velocity_mm_s: "target v (mm/s)",
  target_accel_mm_s2:   "target a (mm/s\u00B2)",
  max_jerk_mm_s3:       "max jerk (mm/s\u00B3)",
  pwm_freq_hz:          "PWM freq (Hz)",
  pwm_compensation:     "PWM comp (0-1)",
  force_scale:          "force scale",
  max_duration_ms:      "timeout ms",
};

const TIMEOUT_MS = 5000;

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((_, reject) => setTimeout(() => reject(new Error(`${label} timed out after ${ms}ms`)), ms)),
  ]);
}

export default function MoveTestWidget({ onStatus }: WidgetProps) {
  const [params, setParams] = useState(DEFAULT_PARAMS);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const updateParam = (key: string, val: string) => {
    setParams(p => ({ ...p, [key]: parseFloat(val) || 0 }));
  };

  const log = (collected: string[], msg: string) => {
    collected.push(msg);
    setLogs([...collected]);
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
      // Step 1: Check board state and ensure piece at (0,3)
      log(collected, "--- SETUP ---");
      onStatus("Checking board state...");

      const state = await withTimeout(getBoardState(), TIMEOUT_MS, "getBoardState");
      const pieces = state.pieces;

      // Check if piece is at (0,3)
      const pieceAt03 = pieces[0]?.[3] ?? 0;
      if (pieceAt03 === 0) {
        log(collected, "No piece at (0,3) — setting up...");

        // Find any piece and move it, or just place one
        let foundPiece = false;
        for (let x = 0; x < pieces.length && !foundPiece; x++) {
          for (let y = 0; y < (pieces[x]?.length ?? 0) && !foundPiece; y++) {
            if (pieces[x][y] !== 0) {
              log(collected, `Found piece at (${x},${y}), sweeping to (0,3)...`);
              // Sweep to column 0 then to row 3
              try {
                if (x > 0) await withTimeout(moveDumb({ from_x: x, from_y: y, to_x: 0, to_y: y }), TIMEOUT_MS, "sweep x");
                if (y !== 3) await withTimeout(moveDumb({ from_x: 0, from_y: y, to_x: 0, to_y: 3 }), TIMEOUT_MS, "sweep y");
                foundPiece = true;
              } catch (e) {
                log(collected, `Sweep failed: ${e}`);
              }
            }
          }
        }

        if (!foundPiece) {
          log(collected, "No pieces found — placing piece at (0,3)");
          await withTimeout(setPiece({ x: 0, y: 3, id: 1 }), TIMEOUT_MS, "setPiece");
        }
      } else {
        log(collected, "Piece already at (0,3)");
      }

      // Step 2: Physics move (0,3) -> (9,3)
      log(collected, "--- PHYSICS MOVE (0,3) -> (9,3) ---");
      onStatus("Physics move (0,3) -> (9,3)...");

      const moveRes = await withTimeout(
        movePhysics({ from_x: 0, from_y: 3, to_x: 9, to_y: 3, ...params }),
        (params.max_duration_ms || 5000) + 2000,  // firmware timeout + 2s buffer
        "movePhysics"
      );

      const moveOk = moveRes.success;
      log(collected, `--- RESULT: ${moveOk ? "SUCCESS" : "FAILED"} ---`);

      // Step 3: Read sensors
      const postState = await withTimeout(getBoardState(), TIMEOUT_MS, "getBoardState post-move");
      const row1 = postState.raw_strengths.map((col: number[]) => col[1]);
      log(collected, `Sensor row y=3: [${row1.join(", ")}]`);

      // Step 4: Sweep back — place piece at (9,3) then sweep to (0,3)
      log(collected, "--- SWEEP BACK ---");
      onStatus("Sweeping back to (0,3)...");

      // Ensure board knows piece is at (9,3) regardless of physics result
      try {
        await withTimeout(setPiece({ x: 9, y: 3, id: 1 }), TIMEOUT_MS, "setPiece 9,3");
        // Clear old position
        await withTimeout(setPiece({ x: 0, y: 3, id: 0 }), TIMEOUT_MS, "clearPiece 0,3");
      } catch (e) {
        log(collected, `setPiece failed: ${e}`);
      }

      // Sweep one coil at a time
      for (let x = 9; x > 0; x--) {
        try {
          await withTimeout(
            moveDumb({ from_x: x, from_y: 3, to_x: x - 1, to_y: 3 }),
            TIMEOUT_MS,
            `dumb move ${x}->${x-1}`
          );
        } catch (e) {
          log(collected, `Sweep ${x}->${x-1} failed: ${e}`);
          // Force piece state forward and continue
          try {
            await withTimeout(setPiece({ x: x, y: 3, id: 0 }), TIMEOUT_MS, "clear");
            await withTimeout(setPiece({ x: x - 1, y: 3, id: 1 }), TIMEOUT_MS, "set");
          } catch {}
        }
      }

      log(collected, "--- SWEEP BACK COMPLETE ---");

      // Step 5: Final state
      const finalState = await withTimeout(getBoardState(), TIMEOUT_MS, "getBoardState final");
      const finalRow = finalState.raw_strengths.map((col: number[]) => col[1]);
      log(collected, `Final sensor row y=3: [${finalRow.join(", ")}]`);

      setResult(moveOk ? "Move succeeded" : "Move failed");
      onStatus(moveOk ? "Test complete - success" : "Test complete - failed");
    } catch (e) {
      log(collected, `ERROR: ${e}`);
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
        Ensures piece at (0,3), runs physics move to (9,3), then sweeps back.
      </div>

      {/* Params grid */}
      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "4px 8px",
        background: "#151525", padding: 8, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        <label style={{ ...labelStyle, fontSize: 10 }}>
          active brake
          <input type="checkbox" checked={!!params.active_brake}
            onChange={e => setParams(p => ({ ...p, active_brake: e.target.checked }))}
            disabled={running} />
        </label>
        <label style={{ ...labelStyle, fontSize: 10 }}>
          all coils equal
          <input type="checkbox" checked={!!params.all_coils_equal}
            onChange={e => setParams(p => ({ ...p, all_coils_equal: e.target.checked }))}
            disabled={running} />
        </label>
        {Object.entries(PARAM_LABELS).map(([key, label]) => (
          <label key={key} style={{ ...labelStyle, fontSize: 10 }}>
            {label}
            <input
              type="number"
              step={key === "max_duration_ms" ? 1 : (key === "mu_static" || key === "mu_kinetic") ? 0.01 : (key === "target_velocity_mm_s" || key === "target_accel_mm_s2") ? 1 : 0.1}
              value={(params as Record<string, number | boolean>)[key] as number}
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
            <div key={i} style={{ color: line.startsWith("---") ? "#4fc3f7" : line.startsWith("Sensor") || line.startsWith("Final") ? "#ff9800" : line.startsWith("ERROR") ? "#ef5350" : "#8f8" }}>
              {line}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
