import { useState } from "react";
import { movePhysics, getBoardState, setPiece } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle } from "./shared";

const TIMEOUT_MS = 8000;

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((_, reject) => setTimeout(() => reject(new Error(`${label} timed out`)), ms)),
  ]);
}

export default function MoveTestWidget({ onStatus }: WidgetProps) {
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const log = (collected: string[], msg: string) => {
    collected.push(msg);
    setLogs([...collected]);
  };

  const runTest = async (distance: number) => {
    setRunning(true);
    setResult(null);
    const collected: string[] = [];
    setLogs([]);

    const toX = distance; // 3, 6, or 9
    onStatus(`Move test: (0,3) -> (${toX},3)...`);

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
      // Setup: place piece at (0,3)
      log(collected, `--- SETUP (0,3) -> (${toX},3) ---`);
      await withTimeout(setPiece({ x: 0, y: 3, id: 1 }), TIMEOUT_MS, "setPiece");

      // Physics move
      log(collected, `--- PHYSICS MOVE (0,3) -> (${toX},3) ---`);
      onStatus(`Physics move (0,3) -> (${toX},3)...`);

      let moveRes = await withTimeout(
        movePhysics({ from_x: 0, from_y: 3, to_x: toX, to_y: 3 }),
        8000,
        "movePhysics"
      );

      // If path blocked, clear board and retry
      if (!moveRes.success && moveRes.error === 5) {
        log(collected, "Path blocked — clearing board and retrying");
        for (let x = 0; x <= 9; x++)
          for (let y = 0; y <= 6; y++)
            try { await setPiece({ x, y, id: 0 }); } catch {}
        await withTimeout(setPiece({ x: 0, y: 3, id: 1 }), TIMEOUT_MS, "setPiece");
        moveRes = await withTimeout(
          movePhysics({ from_x: 0, from_y: 3, to_x: toX, to_y: 3 }),
          8000,
          "movePhysics retry"
        );
      }

      const moveOk = moveRes.success;
      log(collected, `--- RESULT: ${moveOk ? "SUCCESS" : "FAILED"} ---`);

      if (moveRes.diag) {
        const d = moveRes.diag;
        const coilSummary = d.coils.map((c, i) => `coil${i}=${c.detected ? "OK" : "MISS"}(${c.min})`).join(" ");
        log(collected, `Diag: ${coilSummary} checkpoint=${d.checkpoint_ok ? "OK" : "FAIL"}`);
      }

      // Read sensors
      const postState = await withTimeout(getBoardState(), TIMEOUT_MS, "getBoardState");
      const row1 = postState.raw_strengths.map((col: number[]) => col[1]);
      log(collected, `Sensor row y=3: [${row1.join(", ")}]`);

      // Move back with physics
      log(collected, "--- MOVE BACK ---");
      onStatus("Moving back...");

      await new Promise(r => setTimeout(r, 200));

      // Set board state for return move
      await withTimeout(setPiece({ x: 0, y: 3, id: 0 }), TIMEOUT_MS, "clear start");
      await withTimeout(setPiece({ x: toX, y: 3, id: 1 }), TIMEOUT_MS, "setPiece dest");

      const backRes = await withTimeout(
        movePhysics({ from_x: toX, from_y: 3, to_x: 0, to_y: 3 }),
        8000,
        "movePhysics back"
      );
      log(collected, `Move back: ${backRes.success ? "OK" : "FAILED"}`);

      log(collected, "--- DONE ---");
      const finalState = await withTimeout(getBoardState(), TIMEOUT_MS, "final");
      const finalRow = finalState.raw_strengths.map((col: number[]) => col[1]);
      log(collected, `Final sensors: [${finalRow.join(", ")}]`);

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
        Place piece at (0,3). Choose move distance, then sweeps back.
      </div>

      {/* 3 move distance buttons */}
      <div style={{ display: "flex", gap: 8 }}>
        <button onClick={() => runTest(3)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "1 step (0\u21923)"}
        </button>
        <button onClick={() => runTest(6)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "2 steps (0\u21926)"}
        </button>
        <button onClick={() => runTest(9)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "3 steps (0\u21929)"}
        </button>
      </div>

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
            <div key={i} style={{ color: line.startsWith("---") ? "#4fc3f7" : line.startsWith("Sensor") || line.startsWith("Final") || line.startsWith("Diag") ? "#ff9800" : line.startsWith("ERROR") ? "#ef5350" : "#8f8" }}>
              {line}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
