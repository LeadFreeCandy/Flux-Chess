import { useState, useEffect, useRef, useCallback } from "react";
import {
  pulseCoil,
  getBoardState,
  setRgb,
  shutdown,
  type PulseCoilResponse,
  type GetBoardStateResponse,
} from "./generated/api";
import { onSerialLog, type SerialLogEntry } from "./transport";

// ── 3D Wireframe Surface ──────────────────────────────────────

function WireframeSurface({ data }: { data: number[][] }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const W = canvas.width;
    const H = canvas.height;
    ctx.clearRect(0, 0, W, H);

    const cols = data.length;
    const rows = data[0]?.length ?? 0;
    if (cols === 0 || rows === 0) return;

    // Find max for normalization
    let max = 1;
    for (const col of data) for (const v of col) if (v > max) max = v;

    // Isometric projection parameters
    const scaleX = 60;
    const scaleY = 40;
    const scaleZ = 100;
    const originX = W / 2 - ((cols - 1) * scaleX) / 4;
    const originY = H - 60;

    function project(x: number, y: number, z: number): [number, number] {
      const px = originX + (x - y) * scaleX * 0.5;
      const py = originY - (x + y) * scaleY * 0.3 - z * scaleZ;
      return [px, py];
    }

    // Draw grid lines along X
    for (let y = 0; y < rows; y++) {
      ctx.beginPath();
      for (let x = 0; x < cols; x++) {
        const z = data[x][y] / max;
        const [px, py] = project(x, y, z);
        if (x === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.strokeStyle = `hsl(${200 + y * 40}, 80%, 60%)`;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    // Draw grid lines along Y
    for (let x = 0; x < cols; x++) {
      ctx.beginPath();
      for (let y = 0; y < rows; y++) {
        const z = data[x][y] / max;
        const [px, py] = project(x, y, z);
        if (y === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.strokeStyle = `hsl(${200 + x * 30}, 70%, 50%)`;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    // Draw dots at vertices
    for (let x = 0; x < cols; x++) {
      for (let y = 0; y < rows; y++) {
        const z = data[x][y] / max;
        const [px, py] = project(x, y, z);
        ctx.beginPath();
        ctx.arc(px, py, 4, 0, Math.PI * 2);
        const hue = (1 - z) * 240;
        ctx.fillStyle = `hsl(${hue}, 90%, 55%)`;
        ctx.fill();
      }
    }

    // Value labels
    ctx.font = "11px monospace";
    ctx.fillStyle = "#aaa";
    for (let x = 0; x < cols; x++) {
      for (let y = 0; y < rows; y++) {
        const z = data[x][y] / max;
        const [px, py] = project(x, y, z);
        ctx.fillText(String(data[x][y]), px + 6, py - 6);
      }
    }
  }, [data]);

  return <canvas ref={canvasRef} width={500} height={300} style={{
    background: "#0a0a1a",
    borderRadius: 8,
    border: "1px solid #333",
  }} />;
}

// ── Serial Console ────────────────────────────────────────────

function SerialConsole() {
  const [logs, setLogs] = useState<SerialLogEntry[]>([]);
  const bottomRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    onSerialLog((entry) => {
      setLogs((prev) => [...prev.slice(-200), entry]);
    });
  }, []);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [logs]);

  return (
    <div style={{
      background: "#0a0a0a",
      border: "1px solid #333",
      borderRadius: 8,
      padding: 12,
      height: 200,
      overflowY: "auto",
      fontFamily: "monospace",
      fontSize: 12,
    }}>
      {logs.length === 0 && <span style={{ color: "#555" }}>Waiting for serial data...</span>}
      {logs.map((entry, i) => (
        <div key={i} style={{ color: entry.dir === "tx" ? "#4fc3f7" : "#81c784" }}>
          <span style={{ color: "#666" }}>
            {new Date(entry.ts).toLocaleTimeString("en", { hour12: false, fractionalSecondDigits: 3 })}
          </span>
          {" "}
          <span style={{ fontWeight: "bold" }}>{entry.dir === "tx" ? "TX" : "RX"}</span>
          {" "}
          {entry.data}
        </div>
      ))}
      <div ref={bottomRef} />
    </div>
  );
}

// ── Main App ──────────────────────────────────────────────────

function App() {
  const [boardState, setBoardState] = useState<GetBoardStateResponse | null>(null);
  const [polling, setPolling] = useState(false);
  const [pulseResult, setPulseResult] = useState<PulseCoilResponse | null>(null);
  const [pulseX, setPulseX] = useState(0);
  const [pulseY, setPulseY] = useState(0);
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

  const handlePulseCoil = async () => {
    try {
      const res = await pulseCoil({ x: pulseX, y: pulseY, duration_ms: pulseDuration });
      setPulseResult(res);
      setStatus(res.success ? "Pulse sent" : `Pulse error: ${res.error}`);
    } catch (e) { setStatus(`Error: ${e}`); }
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
            <WireframeSurface data={boardState.raw_strengths} />
          ) : (
            <div style={{ color: "#555", padding: 40, textAlign: "center" }}>
              Start polling to see sensor data
            </div>
          )}
        </div>

        {/* Controls Row */}
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 16, marginBottom: 16 }}>
          {/* Pulse Coil */}
          <div style={cardStyle}>
            <h2 style={headingStyle}>Pulse Coil</h2>
            <div style={{ display: "flex", gap: 8, flexWrap: "wrap", alignItems: "center" }}>
              <label style={labelStyle}>
                X <input type="number" value={pulseX} min={0} max={9}
                  style={inputStyle} onChange={(e) => setPulseX(Number(e.target.value))} />
              </label>
              <label style={labelStyle}>
                Y <input type="number" value={pulseY} min={0} max={6}
                  style={inputStyle} onChange={(e) => setPulseY(Number(e.target.value))} />
              </label>
              <label style={labelStyle}>
                ms <input type="number" value={pulseDuration} min={1} max={1000}
                  style={{ ...inputStyle, width: 70 }} onChange={(e) => setPulseDuration(Number(e.target.value))} />
              </label>
              <button onClick={handlePulseCoil} style={btnStyle}>Pulse</button>
            </div>
            {pulseResult && (
              <div style={{ marginTop: 8, fontSize: 12, color: pulseResult.success ? "#81c784" : "#ef5350" }}>
                {pulseResult.success ? "OK" : pulseResult.error}
              </div>
            )}
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
