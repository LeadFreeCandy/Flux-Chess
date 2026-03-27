import { useState, useRef } from "react";
import { movePhysics, setPiece } from "../generated/api";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle } from "./shared";

const TIMEOUT_MS = 8000;

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((_, reject) => setTimeout(() => reject(new Error(`${label} timed out`)), ms)),
  ]);
}

type Outcome = "perfect" | "jittery" | "no_move" | "short" | "overshoot" | "wrong_dir" | "error";

interface TrialResult {
  iteration: number;
  params: Params;
  outcome: Outcome;
  logs: string[];
}

type Params = {
  piece_mass_g: number;
  max_current_a: number;
  mu_static: number;
  mu_kinetic: number;
  target_velocity_mm_s: number;
  target_accel_mm_s2: number;
  max_jerk_mm_s3: number;
  active_brake: boolean;
  pwm_freq_hz: number;
  pwm_compensation: number;
  all_coils_equal: boolean;
  force_scale: number;
  max_duration_ms: number;
};

const INITIAL_PARAMS: Params = {
  piece_mass_g: 4.3,
  max_current_a: 0.5,
  mu_static: 0.3,
  mu_kinetic: 0.2,
  target_velocity_mm_s: 50,
  target_accel_mm_s2: 300,
  max_jerk_mm_s3: 50000,
  active_brake: true,
  pwm_freq_hz: 20000,
  pwm_compensation: 0.2,
  all_coils_equal: false,
  force_scale: 1.0,
  max_duration_ms: 5000,
};

const OUTCOMES: { id: Outcome; label: string; emoji: string; desc: string }[] = [
  { id: "perfect",   label: "Perfect",       emoji: "O", desc: "Piece moved smoothly to destination" },
  { id: "jittery",   label: "Jittery",       emoji: "~", desc: "Piece vibrated/oscillated during move" },
  { id: "no_move",   label: "Didn't move",   emoji: "X", desc: "Piece stayed in place" },
  { id: "short",     label: "Stopped short", emoji: "<", desc: "Moved but didn't reach destination" },
  { id: "overshoot", label: "Overshot",      emoji: ">", desc: "Went past destination" },
  { id: "wrong_dir", label: "Wrong way",     emoji: "!", desc: "Moved in wrong direction" },
  { id: "error",     label: "Error/crash",   emoji: "E", desc: "Firmware error or timeout" },
];

function adjustParams(params: Params, outcome: Outcome): Params {
  const p = { ...params };

  switch (outcome) {
    case "no_move":
      // Need more force — increase current, decrease friction estimate
      p.max_current_a = Math.min(p.max_current_a * 1.5, 5.0);
      p.mu_static = Math.max(p.mu_static * 0.8, 0.05);
      break;

    case "jittery":
      // Oscillating — reduce current, increase kinetic friction, lower velocity
      p.max_current_a = Math.max(p.max_current_a * 0.8, 0.1);
      p.mu_kinetic = Math.min(p.mu_kinetic * 1.3, 1.0);
      p.target_velocity_mm_s = Math.max(p.target_velocity_mm_s * 0.7, 20);
      p.target_accel_mm_s2 = Math.max(p.target_accel_mm_s2 * 0.7, 100);
      break;

    case "short":
      // Not enough force to finish — slightly increase current
      p.max_current_a = Math.min(p.max_current_a * 1.2, 5.0);
      p.mu_kinetic = Math.max(p.mu_kinetic * 0.9, 0.05);
      break;

    case "overshoot":
      // Too fast / not enough braking
      p.target_velocity_mm_s = Math.max(p.target_velocity_mm_s * 0.7, 20);
      p.active_brake = true;
      p.mu_kinetic = Math.min(p.mu_kinetic * 1.2, 1.0);
      break;

    case "wrong_dir":
      // Likely a coil mapping issue, but try reducing current
      p.max_current_a = Math.max(p.max_current_a * 0.5, 0.1);
      break;

    case "error":
      // Reduce everything to be safe
      p.max_current_a = Math.max(p.max_current_a * 0.7, 0.1);
      p.target_velocity_mm_s = Math.max(p.target_velocity_mm_s * 0.7, 20);
      p.max_duration_ms = Math.min(p.max_duration_ms + 2000, 15000);
      break;

    case "perfect":
      // Try to speed it up slightly
      p.target_velocity_mm_s = Math.min(p.target_velocity_mm_s * 1.2, 500);
      break;
  }

  // Round for readability
  p.max_current_a = Math.round(p.max_current_a * 100) / 100;
  p.mu_static = Math.round(p.mu_static * 1000) / 1000;
  p.mu_kinetic = Math.round(p.mu_kinetic * 1000) / 1000;
  p.target_velocity_mm_s = Math.round(p.target_velocity_mm_s);
  p.target_accel_mm_s2 = Math.round(p.target_accel_mm_s2);

  return p;
}

type Step = "idle" | "setup" | "moving" | "ask" | "done";

export default function TuneWidget({ onStatus }: WidgetProps) {
  const [step, setStep] = useState<Step>("idle");
  const [params, setParams] = useState<Params>(INITIAL_PARAMS);
  const [history, setHistory] = useState<TrialResult[]>([]);
  const [logs, setLogs] = useState<string[]>([]);
  const [iteration, setIteration] = useState(0);
  const [moveDirection, setMoveDirection] = useState<"forward" | "back">("forward");
  const logsRef = useRef<string[]>([]);

  const startTuning = () => {
    setHistory([]);
    setIteration(0);
    setParams(INITIAL_PARAMS);
    setStep("setup");
    runTrial(INITIAL_PARAMS, 0);
  };

  const runTrial = async (trialParams: Params, iter: number) => {
    setStep("setup");
    logsRef.current = [];
    setLogs([]);
    onStatus(`Trial ${iter + 1}: setting up...`);

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir === "rx") {
        try {
          const msg = JSON.parse(entry.data);
          if (msg.type === "log" && typeof msg.msg === "string" && msg.msg.startsWith("physics:")) {
            logsRef.current = [...logsRef.current, msg.msg];
            setLogs([...logsRef.current]);

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
      // Determine direction: alternate forward/back
      const dir = iter % 2 === 0 ? "forward" : "back";
      setMoveDirection(dir);

      const from_x = dir === "forward" ? 0 : 3;
      const to_x = dir === "forward" ? 3 : 0;
      const y = 3;

      // Ensure piece is at start position
      onStatus(`Trial ${iter + 1}: placing piece at (${from_x},${y})...`);
      await withTimeout(setPiece({ x: from_x, y, id: 1 }), TIMEOUT_MS, "setPiece start");
      // Clear other position
      await withTimeout(setPiece({ x: to_x, y, id: 0 }), TIMEOUT_MS, "setPiece clear");

      // Run physics move
      setStep("moving");
      onStatus(`Trial ${iter + 1}: moving (${from_x},${y}) -> (${to_x},${y})...`);

      await withTimeout(
        movePhysics({ from_x, from_y: y, to_x, to_y: y, ...trialParams }),
        (trialParams.max_duration_ms || 5000) + 3000,
        "movePhysics"
      );

      setStep("ask");
      onStatus(`Trial ${iter + 1}: How did the move look?`);
    } catch (e) {
      logsRef.current.push(`ERROR: ${e}`);
      setLogs([...logsRef.current]);
      setStep("ask");
      onStatus(`Trial ${iter + 1}: Move errored — how did it look?`);
    }

    unsub();
    window.dispatchEvent(new CustomEvent("fluxchess-sim-pos", { detail: null }));
  };

  const handleOutcome = (outcome: Outcome) => {
    const result: TrialResult = {
      iteration,
      params: { ...params },
      outcome,
      logs: [...logsRef.current],
    };

    const newHistory = [...history, result];
    setHistory(newHistory);

    if (outcome === "perfect") {
      // Check if we've had 2 consecutive perfects
      const lastTwo = newHistory.slice(-2);
      if (lastTwo.length === 2 && lastTwo.every(r => r.outcome === "perfect")) {
        setStep("done");
        onStatus("Tuning complete! Parameters found.");
        return;
      }
    }

    // Adjust params and run next trial
    const newParams = adjustParams(params, outcome);
    setParams(newParams);
    const nextIter = iteration + 1;
    setIteration(nextIter);
    runTrial(newParams, nextIter);
  };

  const applyParams = () => {
    localStorage.setItem("fluxchess_physics_params", JSON.stringify(params));
    onStatus("Parameters saved — enable Physics in Hexapawn widget");
  };

  const paramDiff = (prev: Params, next: Params): string[] => {
    const diffs: string[] = [];
    for (const key of Object.keys(prev) as (keyof Params)[]) {
      if (prev[key] !== next[key]) {
        diffs.push(`${key}: ${prev[key]} -> ${next[key]}`);
      }
    }
    return diffs;
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%", fontSize: 12 }}>

      {step === "idle" && (
        <div style={{ background: "#151525", padding: 16, borderRadius: 8, border: "1px solid #2a2a4a", textAlign: "center" }}>
          <p style={{ margin: "0 0 8px 0", color: "#888" }}>
            Guided physics tuning. Place a piece at (0,3). The tuner will run moves, ask what happened, and adjust parameters automatically.
          </p>
          <button onClick={startTuning} style={{ ...btnStyle, width: "100%" }}>
            Start Tuning
          </button>
        </div>
      )}

      {step === "setup" && (
        <div style={{ textAlign: "center", padding: 16, color: "#4fc3f7" }}>
          Setting up trial {iteration + 1}...
        </div>
      )}

      {step === "moving" && (
        <>
          <div style={{ textAlign: "center", padding: 8, color: "#4fc3f7", fontSize: 14 }}>
            Trial {iteration + 1} — moving {moveDirection === "forward" ? "(0,3) -> (3,3)" : "(3,3) -> (0,3)"}
          </div>
          <div style={{
            flex: 1, minHeight: 100, maxHeight: 200, overflow: "auto",
            background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6,
            padding: 6, fontSize: 10, fontFamily: "monospace", color: "#8f8",
            userSelect: "text",
          }}>
            {logs.map((line, i) => <div key={i}>{line}</div>)}
          </div>
        </>
      )}

      {step === "ask" && (
        <div style={{ background: "#151525", padding: 12, borderRadius: 8, border: "1px solid #2a2a4a" }}>
          <div style={{ marginBottom: 8, color: "#e0e0e0", fontWeight: "bold" }}>
            Trial {iteration + 1} — What happened?
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 6 }}>
            {OUTCOMES.map(o => (
              <button key={o.id} onClick={() => handleOutcome(o.id)} style={{
                ...btnStyle,
                background: o.id === "perfect" ? "#1b5e20" : o.id === "error" ? "#b71c1c" : "#2a2a4a",
                border: "1px solid #3a3a5a",
                textAlign: "left",
                padding: "8px 10px",
                fontSize: 11,
              }}>
                <span style={{ fontWeight: "bold" }}>{o.emoji} {o.label}</span>
                <br />
                <span style={{ color: "#888", fontSize: 10 }}>{o.desc}</span>
              </button>
            ))}
          </div>

          {/* Show current params */}
          <div style={{ marginTop: 8, fontSize: 10, color: "#666", fontFamily: "monospace" }}>
            I={params.max_current_a}A v={params.target_velocity_mm_s}mm/s a={params.target_accel_mm_s2}mm/s²
            μs={params.mu_static} μk={params.mu_kinetic} brake={params.active_brake ? "on" : "off"}
          </div>

          {/* Show last few logs */}
          <div style={{
            marginTop: 6, maxHeight: 100, overflow: "auto",
            background: "#0a0a1a", borderRadius: 4, padding: 4,
            fontSize: 9, fontFamily: "monospace", color: "#8f8", userSelect: "text",
          }}>
            {logs.slice(-10).map((line, i) => <div key={i}>{line}</div>)}
          </div>
        </div>
      )}

      {step === "done" && (
        <div style={{ background: "#1b5e20", padding: 16, borderRadius: 8, border: "1px solid #2e7d32", textAlign: "center" }}>
          <div style={{ fontSize: 16, fontWeight: "bold", color: "#fff", marginBottom: 8 }}>
            Tuning Complete
          </div>
          <div style={{ fontSize: 12, color: "#a5d6a7", marginBottom: 12 }}>
            Found working parameters in {iteration + 1} trials
          </div>
          <div style={{ display: "flex", gap: 8 }}>
            <button onClick={applyParams} style={{ ...btnStyle, flex: 1, background: "#fff", color: "#1b5e20", fontWeight: "bold" }}>
              Apply Parameters
            </button>
            <button onClick={() => {
              const blob = new Blob([JSON.stringify({ params, history }, null, 2)], { type: "application/json" });
              const url = URL.createObjectURL(blob);
              const a = document.createElement("a");
              a.href = url; a.download = `tune-${Date.now()}.json`; a.click();
              URL.revokeObjectURL(url);
            }} style={{ ...btnStyle, flex: 1, background: "transparent", border: "1px solid #a5d6a7", color: "#a5d6a7" }}>
              Download History
            </button>
          </div>
        </div>
      )}

      {/* History timeline */}
      {history.length > 0 && (
        <div style={{
          background: "#0a0a1a", border: "1px solid #2a2a4a", borderRadius: 6,
          padding: 8, maxHeight: 200, overflow: "auto",
        }}>
          <div style={{ fontSize: 10, color: "#666", marginBottom: 4 }}>TRIAL HISTORY</div>
          {history.map((r, i) => {
            const o = OUTCOMES.find(x => x.id === r.outcome)!;
            const changes = i > 0 ? paramDiff(history[i-1].params, r.params) : [];
            return (
              <div key={i} style={{
                display: "flex", gap: 8, alignItems: "center",
                padding: "3px 0", borderBottom: "1px solid #1a1a2a", fontSize: 10,
              }}>
                <span style={{
                  width: 20, height: 20, borderRadius: 4, display: "flex",
                  alignItems: "center", justifyContent: "center", fontWeight: "bold",
                  background: r.outcome === "perfect" ? "#1b5e20" : r.outcome === "error" ? "#b71c1c" : "#2a2a4a",
                  color: "#fff", fontSize: 9,
                }}>{o.emoji}</span>
                <span style={{ color: "#888", minWidth: 30 }}>#{i + 1}</span>
                <span style={{ color: "#ccc" }}>{o.label}</span>
                <span style={{ color: "#555", fontFamily: "monospace", fontSize: 9 }}>
                  I={r.params.max_current_a} v={r.params.target_velocity_mm_s}
                </span>
                {changes.length > 0 && (
                  <span style={{ color: "#4fc3f7", fontSize: 9, fontFamily: "monospace" }}>
                    {changes.join(", ")}
                  </span>
                )}
              </div>
            );
          })}
        </div>
      )}

      {/* Manual override */}
      {step !== "idle" && step !== "done" && (
        <div style={{ display: "flex", gap: 8 }}>
          <button onClick={() => { setStep("done"); onStatus("Tuning stopped"); }}
            style={{ ...btnStyle, flex: 1, background: "#2a2a4a", border: "1px solid #3a3a5a", fontSize: 11 }}>
            Stop & Use Current
          </button>
          <button onClick={() => { setStep("idle"); setHistory([]); onStatus("Tuning reset"); }}
            style={{ ...btnStyle, flex: 1, background: "#2a2a4a", border: "1px solid #3a3a5a", fontSize: 11 }}>
            Reset
          </button>
        </div>
      )}
    </div>
  );
}
