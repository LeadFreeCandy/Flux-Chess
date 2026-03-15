import { useState, useEffect, useRef, useCallback } from "react";
import {
  pulseCoil,
  getBoardState,
  setRgb,
  shutdown,
  type GetBoardStateResponse,
} from "./generated/api";
import { onSerialLog, type SerialLogEntry, type LogLevel } from "./transport";
import SurfacePlot from "./SurfacePlot";

// ── Serial Console ────────────────────────────────────────────

// Filter levels (each includes everything below it):
// 0: Everything (hw + board + io + errors)
// 1: Board + IO + errors (no hw)
// 2: IO only (TX/RX commands, no logs)
// 3: Errors only
const FILTER_LABELS = ["All", "Board+", "IO", "Errors"];
const FILTER_PASS: Record<number, Set<LogLevel>> = {
  0: new Set(["hw", "board", "io", "error"]),
  1: new Set(["board", "io", "error"]),
  2: new Set(["io", "error"]),
  3: new Set(["error"]),
};

const LOG_COLORS: Record<LogLevel, string> = {
  hw: "#7e57c2",
  board: "#ffb74d",
  io: "#81c784",
  error: "#ef5350",
};

function SerialConsole() {
  const [logs, setLogs] = useState<SerialLogEntry[]>([]);
  const [filter, setFilter] = useState(0);

  useEffect(() => {
    return onSerialLog((entry) => {
      setLogs((prev) => [...prev.slice(-500), entry]);
    });
  }, []);

  const containerRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    if (el.scrollHeight - el.scrollTop - el.clientHeight < 60) {
      el.scrollTop = el.scrollHeight;
    }
  }, [logs, filter]);

  const filtered = logs.filter((e) => FILTER_PASS[filter].has(e.level));

  return (
    <div>
      <div style={{ display: "flex", gap: 4, marginBottom: 8 }}>
        {FILTER_LABELS.map((label, i) => (
          <button
            key={i}
            onClick={() => setFilter(i)}
            style={{
              ...btnStyle,
              padding: "3px 10px",
              fontSize: 11,
              background: filter === i ? "#1565c0" : "#1a1a2e",
              border: "1px solid #333",
            }}
          >
            {label}
          </button>
        ))}
      </div>
      <div ref={containerRef} style={{
        background: "#0a0a0a",
        border: "1px solid #333",
        borderRadius: 8,
        padding: 12,
        height: 200,
        overflowY: "auto",
        fontFamily: "monospace",
        fontSize: 12,
      }}>
        {filtered.length === 0 && <span style={{ color: "#555" }}>No messages at this filter level</span>}
        {filtered.map((entry, i) => (
          <div key={i} style={{ color: entry.dir === "tx" ? "#4fc3f7" : LOG_COLORS[entry.level] }}>
            <span style={{ color: "#555" }}>
              {new Date(entry.ts).toLocaleTimeString("en", { hour12: false, fractionalSecondDigits: 3 })}
            </span>
            {" "}
            <span style={{ fontWeight: "bold", minWidth: 40, display: "inline-block" }}>
              {entry.dir === "tx" ? "TX" : entry.level.toUpperCase()}
            </span>
            {" "}
            {entry.data}
          </div>
        ))}
      </div>
    </div>
  );
}

// ── Coil Grid ─────────────────────────────────────────────────

const GRID_COLS = 10;
const GRID_ROWS = 7;
const SR_BLOCK = 3;
const SR_COLS = 4;
const SR_ROWS = 3;

function hasCoil(x: number, y: number): boolean {
  if (x >= GRID_COLS || y >= GRID_ROWS) return false;
  const srCol = Math.floor(x / SR_BLOCK);
  const srRow = Math.floor(y / SR_BLOCK);
  if (srCol >= SR_COLS || srRow >= SR_ROWS) return false;
  const lx = x % SR_BLOCK;
  const ly = y % SR_BLOCK;
  if (ly === 0) return true;           // bottom row of L
  if (lx === 0 && ly > 0) return true; // left column of L
  return false;
}

function CoilGrid({ pulseDuration, onStatus }: {
  pulseDuration: number;
  onStatus: (s: string) => void;
}) {
  const [pulsing, setPulsing] = useState<string | null>(null);

  const handleClick = async (x: number, y: number) => {
    const key = `${x},${y}`;
    setPulsing(key);
    try {
      const res = await pulseCoil({ x, y, duration_ms: pulseDuration });
      onStatus(res.success ? `Pulsed (${x},${y})` : `Error: ${res.error}`);
    } catch (e) {
      onStatus(`Error: ${e}`);
    }
    setPulsing(null);
  };

  return (
    <div style={{ display: "grid", gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`, gap: 3, maxWidth: 520, margin: "0 auto" }}>
      {Array.from({ length: GRID_ROWS }, (_, y) =>
        Array.from({ length: GRID_COLS }, (_, x) => {
          const ry = GRID_ROWS - 1 - y;
          const coil = hasCoil(x, ry);
          const key = `${x},${ry}`;
          const isPulsing = pulsing === key;
          return (
            <button
              key={key}
              disabled={!coil}
              onClick={() => coil && handleClick(x, ry)}
              title={coil ? `Pulse (${x}, ${ry})` : ""}
              style={{
                width: "100%",
                height: 30,
                border: coil ? "1px solid #3a5a7c" : "1px solid transparent",
                borderRadius: 4,
                background: isPulsing ? "#f57f17" : coil ? "#1a3a5c" : "transparent",
                color: coil ? "#7ab8e0" : "transparent",
                fontSize: 9,
                cursor: coil ? "pointer" : "default",
                fontFamily: "inherit",
                padding: 0,
                transition: "background 0.15s",
                userSelect: "none",
              }}
            >
              {coil ? `${x},${ry}` : ""}
            </button>
          );
        })
      )}
    </div>
  );
}

// ── Main App ──────────────────────────────────────────────────

function App() {
  const [boardState, setBoardState] = useState<GetBoardStateResponse | null>(null);
  const [polling, setPolling] = useState(false);
  const [pulseDuration, setPulseDuration] = useState(100);
  const [rgb, setRgbState] = useState({ r: 0, g: 0, b: 0 });
  const [status, setStatus] = useState("Connected");
  const pollingRef = useRef(false);

  const pollBoardState = useCallback(async () => {
    if (!pollingRef.current) return;
    try {
      const res = await getBoardState();
      setBoardState(res);
    } catch (e) {
      setStatus(`Poll error: ${e}`);
    }
    if (pollingRef.current) {
      setTimeout(pollBoardState, 100); // 10Hz
    }
  }, []);

  const togglePolling = () => {
    if (polling) {
      pollingRef.current = false;
      setPolling(false);
    } else {
      pollingRef.current = true;
      setPolling(true);
      pollBoardState();
    }
  };

  const handleSetRgb = async () => {
    try {
      await setRgb(rgb);
      setStatus("RGB set");
    } catch (e) { setStatus(`Error: ${e}`); }
  };

  const handleShutdown = async () => {
    try {
      await shutdown();
      setStatus("Shutdown sent");
    } catch (e) { setStatus(`Error: ${e}`); }
  };

  return (
    <div style={{
      fontFamily: "'SF Mono', 'Fira Code', monospace",
      background: "#111",
      color: "#e0e0e0",
      minHeight: "100vh",
      padding: "24px 32px",
    }}>
      <div style={{ maxWidth: 900, margin: "0 auto" }}>
        {/* Header */}
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 24 }}>
          <h1 style={{ margin: 0, fontSize: 24, color: "#fff" }}>FluxChess</h1>
          <span style={{
            padding: "4px 12px",
            borderRadius: 12,
            fontSize: 12,
            background: "#1b5e20",
            color: "#a5d6a7",
          }}>{status}</span>
        </div>

        {/* Sensor Grid */}
        <div style={cardStyle}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12 }}>
            <h2 style={headingStyle}>Magnetic Field</h2>
            <button onClick={togglePolling} style={{
              ...btnStyle,
              background: polling ? "#b71c1c" : "#1565c0",
            }}>
              {polling ? "Stop Polling" : "Start 10Hz Poll"}
            </button>
          </div>
          {boardState ? (
            <SurfacePlot data={boardState.raw_strengths} />
          ) : (
            <div style={{ color: "#555", padding: 40, textAlign: "center" }}>
              Start polling to see sensor data
            </div>
          )}
        </div>

        {/* Coil Grid */}
        <div style={cardStyle}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12 }}>
            <h2 style={headingStyle}>Coil Grid</h2>
            <label style={labelStyle}>
              Pulse duration
              <input type="number" value={pulseDuration} min={1} max={1000}
                style={{ ...inputStyle, width: 70 }} onChange={(e) => setPulseDuration(Number(e.target.value))} />
              <span style={{ color: "#555" }}>ms</span>
            </label>
          </div>
          <CoilGrid pulseDuration={pulseDuration} onStatus={setStatus} />
        </div>

        {/* RGB */}
        <div style={cardStyle}>
          <h2 style={headingStyle}>RGB Underglow</h2>
          <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
            <label style={labelStyle}>
              R <input type="number" value={rgb.r} min={0} max={255}
                style={{ ...inputStyle, width: 55 }} onChange={(e) => setRgbState({ ...rgb, r: Number(e.target.value) })} />
            </label>
            <label style={labelStyle}>
              G <input type="number" value={rgb.g} min={0} max={255}
                style={{ ...inputStyle, width: 55 }} onChange={(e) => setRgbState({ ...rgb, g: Number(e.target.value) })} />
            </label>
            <label style={labelStyle}>
              B <input type="number" value={rgb.b} min={0} max={255}
                style={{ ...inputStyle, width: 55 }} onChange={(e) => setRgbState({ ...rgb, b: Number(e.target.value) })} />
            </label>
            <button onClick={handleSetRgb} style={btnStyle}>Set</button>
          </div>
          <div style={{
            marginTop: 8,
            height: 8,
            borderRadius: 4,
            background: `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`,
          }} />
        </div>

        {/* Serial Console */}
        <div style={cardStyle}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
            <h2 style={headingStyle}>Serial Console</h2>
            <button onClick={handleShutdown} style={{ ...btnStyle, background: "#b71c1c", fontSize: 11 }}>
              Shutdown ESP32
            </button>
          </div>
          <SerialConsole />
        </div>
      </div>
    </div>
  );
}

// ── Shared Styles ─────────────────────────────────────────────

const cardStyle: React.CSSProperties = {
  background: "#1a1a2e",
  borderRadius: 10,
  padding: 16,
  marginBottom: 16,
  border: "1px solid #2a2a4a",
};

const headingStyle: React.CSSProperties = {
  margin: 0,
  fontSize: 14,
  color: "#888",
  textTransform: "uppercase",
  letterSpacing: 1,
};

const btnStyle: React.CSSProperties = {
  background: "#1565c0",
  color: "#fff",
  border: "none",
  borderRadius: 6,
  padding: "6px 14px",
  cursor: "pointer",
  fontSize: 13,
  fontFamily: "inherit",
};

const inputStyle: React.CSSProperties = {
  width: 45,
  background: "#0d0d1a",
  border: "1px solid #333",
  borderRadius: 4,
  color: "#e0e0e0",
  padding: "4px 6px",
  fontSize: 13,
  fontFamily: "inherit",
};

const labelStyle: React.CSSProperties = {
  fontSize: 12,
  color: "#888",
  display: "flex",
  alignItems: "center",
  gap: 4,
};

export default App;
