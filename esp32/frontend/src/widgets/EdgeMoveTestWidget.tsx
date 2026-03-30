import { useState } from "react";
import { onSerialLog, transport, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULTS = {
  pulse_ms: 100,
  duty: 255,
  delay_ms: 50,
  steps: 4,
};

export default function EdgeMoveTestWidget({ onStatus }: WidgetProps) {
  const [params, setParams] = useState(DEFAULTS);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const update = (key: string, val: string) => {
    setParams(p => ({ ...p, [key]: parseInt(val) || 0 }));
  };

  const run = async (direction: "up" | "down") => {
    setRunning(true);
    setResult(null);
    const collected: string[] = [];
    setLogs([]);

    const label = direction === "up" ? "(1.5,0)\u2192(1.5,3)" : "(1.5,3)\u2192(1.5,0)";
    onStatus(`Edge move: ${label}...`);
    collected.push(`--- EDGE MOVE ${label} ---`);
    setLogs([...collected]);

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("edge:")) {
            collected.push(msg.msg);
            setLogs([...collected]);
          }
        } catch {}
      }
    });

    try {
      const res = await transport.call("edge_move_test", {
        direction,
        pulse_ms: params.pulse_ms,
        duty: params.duty,
        delay_ms: params.delay_ms,
        steps: params.steps,
      }) as { success: boolean };

      collected.push(`--- ${res.success ? "DONE" : "FAILED"} ---`);
      setLogs([...collected]);
      setResult(res.success ? "Edge move complete" : "Edge move failed");
      onStatus(res.success ? "Edge move - done" : "Edge move - failed");
    } catch (e) {
      collected.push(`ERROR: ${e}`);
      setLogs([...collected]);
      setResult(`Error: ${e}`);
    }

    unsub();
    setRunning(false);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%" }}>
      <div style={{ fontSize: 12, color: "#888" }}>
        Move piece along the edge between two coil columns (x=1.5).
        Activates symmetric coil pairs on both sides.
      </div>

      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr", gap: "6px 12px",
        background: "#151525", padding: 8, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        <label style={labelStyle}>pulse ms <input type="number" step={10} value={params.pulse_ms}
          onChange={e => update("pulse_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>duty <input type="number" step={10} value={params.duty}
          onChange={e => update("duty", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>delay ms <input type="number" step={10} value={params.delay_ms}
          onChange={e => update("delay_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>steps <input type="number" step={1} min={1} max={4} value={params.steps}
          onChange={e => update("steps", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
      </div>

      <div style={{ fontSize: 10, color: "#666", background: "#151525", padding: 6, borderRadius: 4 }}>
        Coil pairs per step:<br/>
        Step 1: (0,0)+(3,0) → Step 2: (0,1)+(3,1) → Step 3: (0,2)+(3,2) → Step 4: (0,3)+(3,3)
      </div>

      <div style={{ display: "flex", gap: 8 }}>
        <button onClick={() => run("up")} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "\u2191 Up (1.5,0)\u2192(1.5,3)"}
        </button>
        <button onClick={() => run("down")} disabled={running}
          style={{ ...btnStyle, flex: 1, opacity: running ? 0.5 : 1 }}>
          {running ? "..." : "\u2193 Down (1.5,3)\u2192(1.5,0)"}
        </button>
      </div>

      {result && (
        <div style={{
          padding: 8, borderRadius: 4, fontSize: 13, fontWeight: "bold", textAlign: "center",
          background: result.includes("complete") ? "#1b5e20" : "#b71c1c", color: "#fff",
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
            <div key={i} style={{ color: line.startsWith("---") ? "#4fc3f7" : "#8f8" }}>
              {line}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
