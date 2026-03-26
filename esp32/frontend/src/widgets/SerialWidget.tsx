import { useState, useEffect, useRef } from "react";
import { shutdown } from "../generated/api";
import { onSerialLog, type SerialLogEntry, type LogLevel } from "../transport";
import { type WidgetProps, btnStyle } from "./shared";

const FILTER_PASS: Record<number, Set<LogLevel>> = {
  0: new Set(["hw", "board", "io", "error"]),
  1: new Set(["board", "io", "error"]),
  2: new Set(["io", "error"]),
  3: new Set(["error"]),
};

const LOG_COLORS: Record<LogLevel, string> = { hw: "#7e57c2", board: "#ffb74d", io: "#81c784", error: "#ef5350" };

export default function SerialWidget({ onStatus }: WidgetProps) {
  const [logs, setLogs] = useState<SerialLogEntry[]>([]);
  const [filter, setFilter] = useState(0);
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    return onSerialLog((entry) => setLogs((prev) => [...prev.slice(-500), entry]));
  }, []);

  useEffect(() => {
    const el = containerRef.current;
    if (el && el.scrollHeight - el.scrollTop - el.clientHeight < 60) el.scrollTop = el.scrollHeight;
  }, [logs, filter]);

  const handleShutdown = async () => {
    try { await shutdown(); onStatus("Shutdown sent"); } catch (e) { onStatus(`Error: ${e}`); }
  };

  const filtered = logs.filter((e) => FILTER_PASS[filter].has(e.level));

  return (
    <>
      <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 12 }}>
        <div style={{ display: "flex", gap: 4 }}>
          {["All", "Board+", "IO", "Errors"].map((label, i) => (
            <button key={i} onClick={() => setFilter(i)}
              style={{ ...btnStyle, padding: "3px 10px", fontSize: 11, background: filter === i ? "#1565c0" : "#1a1a2e", border: "1px solid #333" }}
            >{label}</button>
          ))}
        </div>
        <button onClick={() => navigator.clipboard.writeText(filtered.map(e => e.data).join("\n"))}
          style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #333", fontSize: 11 }}>Copy</button>
        <button onClick={handleShutdown} style={{ ...btnStyle, background: "#b71c1c", fontSize: 11 }}>Shutdown ESP32</button>
      </div>
      <div ref={containerRef} style={{ background: "#0a0a0a", border: "1px solid #333", borderRadius: 8, padding: 12, height: 200, overflowY: "auto", fontFamily: "monospace", fontSize: 12, userSelect: "text", cursor: "text" }}>
        {filtered.map((entry, i) => (
          <div key={i} style={{ color: entry.dir === "tx" ? "#4fc3f7" : LOG_COLORS[entry.level] }}>
            <span style={{ color: "#555" }}>{new Date(entry.ts).toLocaleTimeString("en", { hour12: false, fractionalSecondDigits: 3 })}</span>{" "}
            <span style={{ fontWeight: "bold", minWidth: 40, display: "inline-block" }}>{entry.dir === "tx" ? "TX" : entry.level.toUpperCase()}</span>{" "}
            {entry.data}
          </div>
        ))}
      </div>
    </>
  );
}