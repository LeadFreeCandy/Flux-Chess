import { useState, useRef } from "react";
import { calibrate, getCalibration, tunePhysics } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle } from "./shared";

type CalStep = "idle" | "instruct" | "running" | "done";

interface CalData {
  valid: boolean;
  sensors: {
    baseline_mean: number;
    baseline_stddev: number;
    piece_mean: number;
    piece_stddev: number;
    piece_median: number;
  }[];
  detections: number[][];
}

export default function CalibrationWidget({ onStatus }: WidgetProps) {
  const [step, setStep] = useState<CalStep>("idle");
  const [logs, setLogs] = useState<string[]>([]);
  const [calData, setCalData] = useState<CalData | null>(null);
  const [showViz, setShowViz] = useState(false);
  const logsRef = useRef<string[]>([]);
  const logBoxRef = useRef<HTMLDivElement>(null);

  const [tuneStep, setTuneStep] = useState<"idle" | "running" | "done">("idle");
  const [tuneLogs, setTuneLogs] = useState<string[]>([]);
  const [tuneResult, setTuneResult] = useState<any>(null);
  const tuneLogsRef = useRef<string[]>([]);

  const startCalibration = async () => {
    setStep("running");
    setCalData(null);
    setShowViz(false);
    logsRef.current = [];
    setLogs([]);
    onStatus("Calibrating...");

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("CAL:")) {
            logsRef.current = [...logsRef.current, msg.msg];
            setLogs(logsRef.current);
            if (logBoxRef.current) {
              logBoxRef.current.scrollTop = logBoxRef.current.scrollHeight;
            }
          }
        } catch {}
      }
    });

    try {
      const res = await calibrate();
      if (res.success) {
        onStatus("Calibration complete");
        const data = (await getCalibration()) as unknown as CalData;
        if (data.valid) setCalData(data);
      } else {
        onStatus("Calibration failed");
      }
    } catch (e) {
      onStatus(`Calibration error: ${e}`);
    }

    unsub();
    setStep("done");
  };

  const fetchCalData = async () => {
    try {
      const data = (await getCalibration()) as unknown as CalData;
      if (data.valid) {
        setCalData(data);
        setShowViz(true);
        onStatus("Calibration data loaded");
      } else {
        onStatus("No calibration data on device");
      }
    } catch (e) {
      onStatus(`Failed to fetch: ${e}`);
    }
  };

  const downloadJson = () => {
    if (!calData) return;
    const blob = new Blob([JSON.stringify(calData, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `calibration-${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const startTuning = async () => {
    setTuneStep("running");
    tuneLogsRef.current = [];
    setTuneLogs([]);
    setTuneResult(null);
    onStatus("Tuning physics...");

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("TUNE:")) {
            tuneLogsRef.current = [...tuneLogsRef.current, msg.msg];
            setTuneLogs(tuneLogsRef.current);
          }
        } catch {}
      }
    });

    try {
      const res = await tunePhysics();
      setTuneResult(res);
      onStatus("Tuning complete");
    } catch (e) {
      onStatus(`Tuning error: ${e}`);
    }

    unsub();
    setTuneStep("done");
  };

  const applyRecommended = () => {
    if (!tuneResult?.recommended) return;
    localStorage.setItem('fluxchess_physics_params', JSON.stringify(tuneResult.recommended));
    onStatus("Recommended params saved — enable Physics in Hexapawn widget");
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 12, height: "100%" }}>

      {step === "idle" && (
        <>
          <div style={{ background: "#151525", padding: 16, borderRadius: 8, border: "1px solid #2a2a4a", textAlign: "center" }}>
            <p style={{ margin: "0 0 12px 0", fontSize: 12, color: "#888" }}>
              Place a single magnetic piece on the bottom-left corner (0,0) of the board, then start calibration.
            </p>
            <div style={{ display: "flex", gap: 8 }}>
              <button onClick={() => setStep("instruct")} style={{ ...btnStyle, flex: 1 }}>
                Start Calibration
              </button>
              <button onClick={fetchCalData} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>
                View Stored
              </button>
              <button onClick={startTuning} style={{ ...btnStyle, background: "#1a237e", flex: 1 }}>
                Tune Physics
              </button>
            </div>
          </div>

          {showViz && calData && <CalViz data={calData} />}
        </>
      )}

      {step === "instruct" && (
        <div style={{ background: "#1b5e20", padding: 16, borderRadius: 8, border: "1px solid #2e7d32", textAlign: "center" }}>
          <h3 style={{ margin: "0 0 8px 0", fontSize: 15, color: "#fff" }}>Confirm Piece Placement</h3>
          <p style={{ margin: "0 0 16px 0", fontSize: 13, color: "#a5d6a7" }}>
            Ensure a single piece is on <strong>(0, 0)</strong> and all other pieces are removed.
          </p>
          <div style={{ display: "flex", gap: 8 }}>
            <button onClick={() => setStep("idle")} style={{ ...btnStyle, background: "transparent", border: "1px solid #a5d6a7", color: "#a5d6a7", flex: 1 }}>
              Cancel
            </button>
            <button onClick={startCalibration} style={{ ...btnStyle, background: "#fff", color: "#1b5e20", fontWeight: "bold", flex: 2 }}>
              Piece is Ready
            </button>
          </div>
        </div>
      )}

      {(step === "running" || step === "done") && (
        <>
          <div
            ref={logBoxRef}
            style={{
              flex: 1, minHeight: 150, maxHeight: 300, overflow: "auto",
              background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6,
              padding: 8, fontSize: 11, fontFamily: "monospace", color: "#8f8",
              userSelect: "text", cursor: "text",
            }}
          >
            {logs.length === 0 && step === "running" && (
              <div style={{ color: "#888" }}>Waiting for calibration data...</div>
            )}
            {logs.map((line, i) => <div key={i}>{line}</div>)}
          </div>

          {step === "running" && (
            <div style={{ textAlign: "center", color: "#4fc3f7", fontSize: 13 }}>
              Calibrating... do not move the board.
            </div>
          )}

          {step === "done" && (
            <>
              <div style={{ display: "flex", gap: 8 }}>
                <button onClick={() => navigator.clipboard.writeText(logs.join("\n"))} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>Copy Log</button>
                <button onClick={downloadJson} style={{ ...btnStyle, flex: 1 }}>Download JSON</button>
                <button onClick={() => setShowViz(v => !v)} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>
                  {showViz ? "Hide" : "Show"} Details
                </button>
                <button onClick={() => { setStep("idle"); setLogs([]); setCalData(null); setShowViz(false); }}
                  style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>
                  Reset
                </button>
              </div>
              {showViz && calData && <CalViz data={calData} />}
            </>
          )}
        </>
      )}

      {tuneStep === "running" && (
        <div style={{ background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6, padding: 8, fontSize: 11, fontFamily: "monospace", color: "#8f8", maxHeight: 300, overflow: "auto", userSelect: "text", cursor: "text" }}>
          {tuneLogs.length === 0 ? <div style={{ color: "#888" }}>Waiting for tuning data...</div> : null}
          {tuneLogs.map((line, i) => <div key={i}>{line}</div>)}
        </div>
      )}

      {tuneStep === "done" && tuneResult && (
        <div style={{ background: "#151525", padding: 12, borderRadius: 8, border: "1px solid #2a2a4a" }}>
          <div style={{ fontSize: 12, color: "#888", marginBottom: 8 }}>Tuning Results</div>
          {tuneResult.verification && (
            <div style={{ marginBottom: 8, fontSize: 12, color: tuneResult.verification.success_count >= 3 ? "#4caf50" : "#ef5350" }}>
              Verification: {tuneResult.verification.success_count}/{tuneResult.verification.total} moves succeeded
            </div>
          )}
          <div style={{ display: "flex", gap: 8 }}>
            <button onClick={applyRecommended} style={{ ...btnStyle, flex: 1 }}>Apply Recommended</button>
            <button onClick={() => { const blob = new Blob([JSON.stringify(tuneResult, null, 2)], { type: "application/json" }); const url = URL.createObjectURL(blob); const a = document.createElement("a"); a.href = url; a.download = `tune-${Date.now()}.json`; a.click(); URL.revokeObjectURL(url); }} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>Download JSON</button>
            <button onClick={() => { setTuneStep("idle"); setTuneLogs([]); setTuneResult(null); }} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", flex: 1 }}>Reset</button>
          </div>
        </div>
      )}
    </div>
  );
}

function CalViz({ data }: { data: CalData }) {
  const cols = data.detections.length;    // 4
  const rows = data.detections[0]?.length || 0; // 3

  return (
    <div style={{ background: "#151525", padding: 12, borderRadius: 8, border: "1px solid #2a2a4a" }}>
      {/* Detection Grid */}
      <div style={{ fontSize: 12, color: "#888", marginBottom: 8 }}>Detection Grid</div>
      <div style={{
        display: "grid",
        gridTemplateColumns: `repeat(${cols}, 1fr)`,
        gap: 4, marginBottom: 16,
      }}>
        {Array.from({ length: rows }, (_, r) => r).map(row =>
          Array.from({ length: cols }, (_, col) => {
            const det = data.detections[col]?.[row] ?? 0;
            return (
              <div key={`${col}-${row}`} style={{
                padding: "8px 4px", textAlign: "center", borderRadius: 4, fontSize: 13, fontWeight: "bold",
                background: det ? "#1b5e20" : "#3a1a1a",
                color: det ? "#4caf50" : "#ef5350",
                border: `1px solid ${det ? "#2e7d32" : "#5a2a2a"}`,
              }}>
                {det ? "OK" : "MISS"}
              </div>
            );
          })
        )}
      </div>

      {/* Sensor Table */}
      <div style={{ fontSize: 12, color: "#888", marginBottom: 8 }}>Sensor Data</div>
      <div style={{ overflowX: "auto" }}>
        <table style={{ width: "100%", fontSize: 11, borderCollapse: "collapse", fontFamily: "monospace" }}>
          <thead>
            <tr style={{ color: "#888", borderBottom: "1px solid #2a2a4a" }}>
              <th style={{ padding: "4px 6px", textAlign: "left" }}>#</th>
              <th style={{ padding: "4px 6px", textAlign: "right" }}>Base</th>
              <th style={{ padding: "4px 6px", textAlign: "right" }}>Piece</th>
              <th style={{ padding: "4px 6px", textAlign: "right" }}>Delta</th>
              <th style={{ padding: "4px 6px", textAlign: "right" }}>StdDev</th>
            </tr>
          </thead>
          <tbody>
            {data.sensors.map((s, i) => {
              const delta = s.baseline_mean - s.piece_mean;
              const broken = s.baseline_mean < 100;
              return (
                <tr key={i} style={{ borderBottom: "1px solid #1a1a2a", color: broken ? "#555" : "#ccc" }}>
                  <td style={{ padding: "3px 6px" }}>{i}</td>
                  <td style={{ padding: "3px 6px", textAlign: "right" }}>{s.baseline_mean.toFixed(0)}</td>
                  <td style={{ padding: "3px 6px", textAlign: "right" }}>{s.piece_mean.toFixed(0)}</td>
                  <td style={{ padding: "3px 6px", textAlign: "right", color: delta > 50 ? "#4caf50" : broken ? "#555" : "#ef5350" }}>
                    {delta.toFixed(0)}
                  </td>
                  <td style={{ padding: "3px 6px", textAlign: "right" }}>{s.piece_stddev.toFixed(1)}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}
