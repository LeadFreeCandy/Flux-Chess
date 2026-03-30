import { useState } from "react";
import { diagonalTest, setPiece } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULTS = {
  catapult_ms: 30,
  catapult_duty: 255,
  catch_ms: 200,
  catch_duty: 255,
  center_ms: 100,
};

export default function DiagonalTestWidget({ onStatus }: WidgetProps) {
  const [params, setParams] = useState(DEFAULTS);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const update = (key: string, val: string) => {
    setParams(p => ({ ...p, [key]: parseInt(val) || 0 }));
  };

  const run = async (fromX: number, fromY: number, toX: number, toY: number) => {
    setRunning(true);
    setResult(null);
    const collected: string[] = [];
    setLogs([]);

    const label = `(${fromX},${fromY})\u2192(${toX},${toY})`;
    onStatus(`Diagonal test: ${label}...`);

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("diag:")) {
            collected.push(msg.msg);
            setLogs([...collected]);
          }
        } catch {}
      }
    });

    try {
      // Setup: place piece at origin
      await setPiece({ x: fromX, y: fromY, id: 1 });
      await setPiece({ x: toX, y: toY, id: 0 });

      collected.push(`--- DIAGONAL ${label} ---`);
      setLogs([...collected]);

      const res = await diagonalTest({
        from_x: fromX, from_y: fromY, to_x: toX, to_y: toY,
        ...params,
      });

      const ok = res.success;
      collected.push(`--- ${ok ? "SUCCESS" : "FAILED"} ---`);
      if (res.error) collected.push(`Error: ${res.error}`);
      setLogs([...collected]);
      setResult(ok ? "Diagonal succeeded" : `Failed: ${res.error || "unknown"}`);
      onStatus(ok ? "Diagonal test - success" : "Diagonal test - failed");
    } catch (e) {
      collected.push(`ERROR: ${e}`);
      setLogs([...collected]);
      setResult(`Error: ${e}`);
      onStatus(`Diagonal test error: ${e}`);
    }

    unsub();
    setRunning(false);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%" }}>
      <div style={{ fontSize: 12, color: "#888" }}>
        Test diagonal moves using symmetric coil pairs + momentum.
      </div>

      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr", gap: "6px 12px",
        background: "#151525", padding: 8, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        <label style={labelStyle}>catapult ms <input type="number" step={5} value={params.catapult_ms}
          onChange={e => update("catapult_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>catapult duty <input type="number" step={10} value={params.catapult_duty}
          onChange={e => update("catapult_duty", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>catch ms <input type="number" step={10} value={params.catch_ms}
          onChange={e => update("catch_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>catch duty <input type="number" step={10} value={params.catch_duty}
          onChange={e => update("catch_duty", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>center ms <input type="number" step={10} value={params.center_ms}
          onChange={e => update("center_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
      </div>

      <div style={{ display: "flex", gap: 8 }}>
        <button onClick={() => run(0, 0, 3, 3)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "(0,0)\u2192(3,3)"}
        </button>
        <button onClick={() => run(3, 3, 0, 0)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "(3,3)\u2192(0,0)"}
        </button>
        <button onClick={() => run(0, 6, 3, 3)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "(0,6)\u2192(3,3)"}
        </button>
        <button onClick={() => run(6, 0, 3, 3)} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "(6,0)\u2192(3,3)"}
        </button>
      </div>

      {result && (
        <div style={{
          padding: 8, borderRadius: 4, fontSize: 13, fontWeight: "bold", textAlign: "center",
          background: result.includes("success") ? "#1b5e20" : "#b71c1c", color: "#fff",
        }}>{result}</div>
      )}

      {logs.length > 0 && (
        <div style={{
          flex: 1, minHeight: 100, maxHeight: 300, overflow: "auto",
          background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6,
          padding: 8, fontSize: 10, fontFamily: "monospace", color: "#8f8",
          userSelect: "text", cursor: "text",
        }}>
          {logs.map((line, i) => (
            <div key={i} style={{ color: line.startsWith("---") ? "#4fc3f7" : line.startsWith("diag:") ? "#8f8" : "#ef5350" }}>
              {line}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
