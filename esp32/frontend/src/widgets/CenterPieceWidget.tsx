import { useState } from "react";
import { onSerialLog, transport, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULTS = {
  x: 3,
  y: 3,
  center1_ms: 250,
  adj_ms: 40,
  between_ms: 100,
  axis_switch_ms: 200,
  adj_repeats: 1,
  center2_ms: 200,
};

export default function CenterPieceWidget({ onStatus }: WidgetProps) {
  const [params, setParams] = useState(DEFAULTS);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);

  const update = (key: keyof typeof DEFAULTS, val: string) => {
    setParams(p => ({ ...p, [key]: parseInt(val) || 0 }));
  };

  const run = async () => {
    setRunning(true);
    setResult(null);
    const collected: string[] = [];
    setLogs([]);

    const label = `(${params.x},${params.y})`;
    onStatus(`Center piece at ${label}...`);
    collected.push(`--- CENTER PIECE ${label} ---`);
    setLogs([...collected]);

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("center:")) {
            collected.push(msg.msg);
            setLogs([...collected]);
          }
        } catch {}
      }
    });

    try {
      const res = await transport.call("center_piece", {
        x: params.x,
        y: params.y,
        center1_ms: params.center1_ms,
        adj_ms: params.adj_ms,
        between_ms: params.between_ms,
        axis_switch_ms: params.axis_switch_ms,
        adj_repeats: params.adj_repeats,
        center2_ms: params.center2_ms,
      }) as { success: boolean; detected: boolean; detected_at: string; error?: string };

      const tag = res.success
        ? (res.detected ? `DETECTED at ${res.detected_at}` : "NOT DETECTED")
        : `FAILED: ${res.error ?? ""}`;
      collected.push(`--- ${tag} ---`);
      setLogs([...collected]);
      setResult(tag);
      onStatus(`Center piece - ${tag}`);
    } catch (e) {
      collected.push(`ERROR: ${e}`);
      setLogs([...collected]);
      setResult(`Error: ${e}`);
    }

    unsub();
    setRunning(false);
  };

  const resultColor = result == null
    ? "#333"
    : result.startsWith("DETECTED") ? "#1b5e20"
    : result.startsWith("NOT") ? "#ef6c00"
    : "#b71c1c";

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%" }}>
      <div style={{ fontSize: 12, color: "#888" }}>
        Pulse center coil, then W/E/N/S adjacent coils, then center again.
        Polls sensor between pulses; returns early on detection.
      </div>

      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr", gap: "6px 12px",
        background: "#151525", padding: 8, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        <label style={labelStyle}>x <input type="number" step={3} min={0} max={9} value={params.x}
          onChange={e => update("x", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>y <input type="number" step={3} min={0} max={6} value={params.y}
          onChange={e => update("y", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>center1 ms <input type="number" step={10} value={params.center1_ms}
          onChange={e => update("center1_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>adj ms <input type="number" step={5} value={params.adj_ms}
          onChange={e => update("adj_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>between ms <input type="number" step={5} value={params.between_ms}
          onChange={e => update("between_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>axis switch ms <input type="number" step={5} value={params.axis_switch_ms}
          onChange={e => update("axis_switch_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>adj repeats <input type="number" step={1} min={0} max={20} value={params.adj_repeats}
          onChange={e => update("adj_repeats", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
        <label style={labelStyle}>center2 ms <input type="number" step={10} value={params.center2_ms}
          onChange={e => update("center2_ms", e.target.value)} style={{ ...inputStyle, width: 55 }} disabled={running} /></label>
      </div>

      <div style={{ fontSize: 10, color: "#666", background: "#151525", padding: 6, borderRadius: 4 }}>
        Major-grid coords: x &isin; {"{0,3,6,9}"}, y &isin; {"{0,3,6}"}.
        Adjacent = (x&plusmn;1,y) and (x,y&plusmn;1); off-grid neighbors are skipped.
      </div>

      <button onClick={run} disabled={running}
        style={{ ...btnStyle, opacity: running ? 0.5 : 1 }}>
        {running ? "..." : `Center at (${params.x},${params.y})`}
      </button>

      {result && (
        <div style={{
          padding: 8, borderRadius: 4, fontSize: 13, fontWeight: "bold", textAlign: "center",
          background: resultColor, color: "#fff",
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
