import { useState, useEffect } from "react";
import { getBoardState, moveDumb, movePhysics, type GetBoardStateResponse } from "../generated/api";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const PIECE_NONE = 0;
const PIECE_WHITE = 1;
const PIECE_BLACK = 2;

const MAJOR_COLS = [0, 3, 6];
const MAJOR_ROWS = [0, 3, 6];
const GRAVE_COL = 9;
const ALL_COLS = [...MAJOR_COLS, GRAVE_COL];

type Pos = { x: number; y: number };

const DEFAULT_PHYSICS_PARAMS = {
  force_k: 10.0,
  force_epsilon: 0.3,
  falloff_exp: 2.0,
  voltage_scale: 1.0,
  friction_static: 3.0,
  friction_kinetic: 2.0,
  target_velocity: 5.0,
  target_accel: 20.0,
  sensor_k: 500.0,
  sensor_falloff: 2.0,
  sensor_threshold: 50.0,
  manual_baseline: 2030,
  manual_piece_mean: 1700,
  max_duration_ms: 5000,
};

const PARAM_INFO: Record<keyof typeof DEFAULT_PHYSICS_PARAMS, string> = {
  force_k:           "force strength (arb)",
  force_epsilon:     "force min dist (gu)",
  falloff_exp:       "force falloff exp",
  voltage_scale:     "voltage mult (1=nom)",
  friction_static:   "static fric (force)",
  friction_kinetic:  "kinetic fric (force/v)",
  target_velocity:   "max speed (gu/s)",
  target_accel:      "max accel (gu/s\u00B2)",
  sensor_k:          "sensor strength (arb)",
  sensor_falloff:    "sensor falloff exp",
  sensor_threshold:  "sensor thresh (ADC)",
  manual_baseline:   "baseline (ADC)",
  manual_piece_mean: "piece mean (ADC)",
  max_duration_ms:   "timeout (ms)",
};

function defaultPieces(): number[][] {
  const grid: number[][] = Array.from({ length: 10 }, () => Array(7).fill(PIECE_NONE));
  grid[0][0] = PIECE_WHITE; grid[3][0] = PIECE_WHITE; grid[6][0] = PIECE_WHITE;
  grid[0][6] = PIECE_BLACK; grid[3][6] = PIECE_BLACK; grid[6][6] = PIECE_BLACK;
  return grid;
}

export default function HexapawnWidget({ onStatus }: WidgetProps) {
  const [boardState, setBoardState] = useState<GetBoardStateResponse | null>(null);
  const [selected, setSelected] = useState<Pos | null>(null);
  const [isMoving, setIsMoving] = useState(false);
  const [usePhysics, setUsePhysics] = useState(false);
  const [showParams, setShowParams] = useState(false);
  const [physicsParams, setPhysicsParams] = useState(DEFAULT_PHYSICS_PARAMS);

  useEffect(() => {
    const saved = localStorage.getItem('fluxchess_physics_params');
    if (saved) {
      try {
        const parsed = JSON.parse(saved);
        setPhysicsParams(p => ({ ...p, ...parsed }));
        localStorage.removeItem('fluxchess_physics_params');  // consume once
      } catch {}
    }
  }, []);

  const fetchBoard = async () => {
    try {
      const res = await getBoardState();
      setBoardState(res);
    } catch {
      if (!boardState) {
        setBoardState({ raw_strengths: [], pieces: defaultPieces() });
      }
    }
  };

  useEffect(() => { fetchBoard(); }, []);

  const pieces = boardState?.pieces;

  const getPiece = (x: number, y: number): number => {
    if (!pieces || x >= pieces.length || y >= pieces[0].length) return PIECE_NONE;
    return pieces[x][y];
  };

  const handleClick = async (x: number, y: number) => {
    if (isMoving || !pieces) return;

    if (selected) {
      if (selected.x === x && selected.y === y) {
        setSelected(null);
        return;
      }

      setIsMoving(true);
      const mode = usePhysics ? "physics" : "dumb";
      onStatus(`Moving ${mode} (${selected.x},${selected.y}) → (${x},${y})...`);

      try {
        const moveParams = { from_x: selected.x, from_y: selected.y, to_x: x, to_y: y };
        const res = usePhysics
          ? await movePhysics({ ...moveParams, ...physicsParams })
          : await moveDumb(moveParams);
        if (res.success) {
          onStatus(`Moved to (${x},${y})`);
        } else {
          onStatus(`Move failed: ${res.error}`);
        }
      } catch (e) {
        onStatus(`Error: ${e}`);
      }

      await fetchBoard();
      setSelected(null);
      setIsMoving(false);
      return;
    }

    if (getPiece(x, y) !== PIECE_NONE) {
      setSelected({ x, y });
    }
  };

  const updateParam = (key: keyof typeof physicsParams, val: string) => {
    setPhysicsParams(p => ({ ...p, [key]: parseFloat(val) || 0 }));
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", alignItems: "center" }}>

      <div style={{ display: "flex", justifyContent: "space-between", width: "100%", maxWidth: 400, marginBottom: 8, alignItems: "center", gap: 8 }}>
        <div style={{ fontSize: 13, color: "#888", flex: 1 }}>
          {selected ? `(${selected.x},${selected.y}) → click dest` : "Click piece"}
        </div>
        <label style={{ ...labelStyle, cursor: "pointer" }}>
          <input type="checkbox" checked={usePhysics} onChange={e => setUsePhysics(e.target.checked)} />
          Physics
        </label>
        {usePhysics && (
          <button style={{ ...btnStyle, fontSize: 11, padding: "3px 8px", background: "#2a2a4a", border: "1px solid #3a3a5a" }}
            onClick={() => setShowParams(v => !v)}>
            {showParams ? "Hide" : "Tune"}
          </button>
        )}
        <button style={btnStyle} onClick={fetchBoard}>Refresh</button>
      </div>

      {usePhysics && showParams && (
        <div style={{
          display: "grid", gridTemplateColumns: "1fr 1fr", gap: "4px 12px",
          width: "100%", maxWidth: 400, marginBottom: 8,
          background: "#151525", padding: 10, borderRadius: 6, border: "1px solid #2a2a4a",
        }}>
          {(Object.entries(PARAM_INFO) as [keyof typeof DEFAULT_PHYSICS_PARAMS, string][]).map(([key, label]) => (
            <label key={key} style={labelStyle}>
              {label}
              <input
                type="number"
                step={['max_duration_ms', 'manual_baseline', 'manual_piece_mean'].includes(key) ? 1 : 0.1}
                value={physicsParams[key]}
                onChange={e => updateParam(key, e.target.value)}
                style={{ ...inputStyle, width: 70 }}
              />
            </label>
          ))}
        </div>
      )}

      {usePhysics && showParams && <PhysicsDebug p={physicsParams} />}

      <div style={{
        display: "grid",
        gridTemplateColumns: `repeat(4, 70px)`,
        gridTemplateRows: `repeat(3, 70px)`,
        gap: 4, background: "#222", padding: 6, borderRadius: 8,
      }}>
        {MAJOR_ROWS.slice().reverse().map((gy) =>
          ALL_COLS.map((gx) => {
            const piece = getPiece(gx, gy);
            const isSel = selected?.x === gx && selected?.y === gy;
            const isGrave = gx === GRAVE_COL;
            const isDark = ((gx / 3) + (gy / 3)) % 2 === 1;

            return (
              <div
                key={`${gx}-${gy}`}
                onClick={() => handleClick(gx, gy)}
                style={{
                  width: 70, height: 70,
                  background: isGrave ? "#1a1a2e" : isDark ? "#3a5a7c" : "#D7BA89",
                  border: isGrave ? "2px dashed #333" : "none",
                  boxShadow: isSel ? "inset 0 0 0 4px #f57f17" : "none",
                  display: "flex", justifyContent: "center", alignItems: "center",
                  fontSize: 44,
                  color: piece === PIECE_BLACK ? "#333" : "#fff",
                  WebkitTextStroke: piece !== PIECE_NONE ? "1px #888" : undefined,
                  cursor: isMoving ? "default" : "pointer",
                  userSelect: "none", borderRadius: 4,
                  opacity: isMoving ? 0.6 : isGrave ? 0.7 : 1,
                }}
              >
                {piece === PIECE_WHITE ? "♟" : piece === PIECE_BLACK ? "♟" : ""}
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}

function PhysicsDebug({ p }: { p: typeof DEFAULT_PHYSICS_PARAMS }) {
  const { force_k, force_epsilon: eps, falloff_exp: exp, voltage_scale: vs,
          friction_static, friction_kinetic, target_velocity: tv, target_accel: ta,
          sensor_k, sensor_falloff, sensor_threshold, manual_baseline, manual_piece_mean,
          max_duration_ms } = p;

  // Peak force distance: d where d/(d^exp + eps) is maximized
  const d_peak = Math.pow(eps, 1 / exp);
  const f_peak = vs * force_k * d_peak / (Math.pow(d_peak, exp) + eps);

  // Force at 1 grid unit (adjacent coil)
  const f_at_1 = vs * force_k * 1.0 / (Math.pow(1.0, exp) + eps);

  // Time to cross board (9 units at target velocity)
  const t_cross = 9.0 / tv;

  // Time to reach target velocity from rest
  const t_accel = tv / ta;

  // Distance covered during acceleration
  const d_accel = 0.5 * ta * t_accel * t_accel;

  // Sensor activation range: solve sensor_k / (d^sensor_falloff + eps) = sensor_threshold
  // → d = ((sensor_k / sensor_threshold) - eps) ^ (1/sensor_falloff)
  const s_ratio = sensor_k / sensor_threshold;
  const d_sensor = s_ratio > eps ? Math.pow(s_ratio - eps, 1 / sensor_falloff) : 0;

  // Sensor range at full detection (reading = piece_mean, strength ≈ sensor_k at d≈0)
  // Basically d_sensor is max detection range

  // Braking distance: v^2 / (2 * friction_kinetic) — rough, ignoring coil force
  const d_brake = (tv * tv) / (2 * friction_kinetic);

  const row = (label: string, value: string) => (
    <div style={{ display: "flex", justifyContent: "space-between" }}>
      <span style={{ color: "#888" }}>{label}</span>
      <span style={{ color: "#e0e0e0" }}>{value}</span>
    </div>
  );

  return (
    <div style={{
      width: "100%", maxWidth: 400, marginBottom: 8,
      background: "#0a0a1a", padding: 10, borderRadius: 6, border: "1px solid #2a2a4a",
      fontSize: 11, fontFamily: "monospace", display: "flex", flexDirection: "column", gap: 2,
    }}>
      <div style={{ color: "#666", fontSize: 10, marginBottom: 4 }}>DERIVED VALUES (gu = grid unit, 1 gu = coil spacing)</div>
      {row("Peak force", `${f_peak.toFixed(1)} at d=${d_peak.toFixed(2)} gu`)}
      {row("Force at adjacent coil (1 gu)", `${f_at_1.toFixed(1)}`)}
      {row("Can overcome stiction?", `${f_peak.toFixed(1)} / ${friction_static.toFixed(1)} = ${(f_peak / friction_static).toFixed(1)}x`)}
      {row("Friction at max speed", `${(friction_kinetic * tv).toFixed(1)} (${((friction_kinetic * tv) / f_peak * 100).toFixed(0)}% of peak force)`)}
      {row("Board traverse (9 gu)", `${t_cross.toFixed(2)}s at ${tv.toFixed(1)} gu/s`)}
      {row("0 to max speed", `${(t_accel * 1000).toFixed(0)}ms over ${d_accel.toFixed(2)} gu`)}
      {row("Friction-only braking", `${d_brake.toFixed(1)} gu (coils help in practice)`)}
      {row("Sensor detect range", `${d_sensor.toFixed(1)} gu from sensor center`)}
      {row("Manual cal", `${manual_baseline.toFixed(0)} → ${manual_piece_mean.toFixed(0)} ADC (delta ${(manual_baseline - manual_piece_mean).toFixed(0)})`)}
      {f_peak < friction_static && (
        <div style={{ color: "#ef5350", marginTop: 4 }}>
          Peak force ({f_peak.toFixed(2)}) &lt; static friction ({friction_static}) — piece won't move!
        </div>
      )}
      {d_brake > 1.5 && (
        <div style={{ color: "#ff9800", marginTop: 4 }}>
          Braking distance ({d_brake.toFixed(1)}u) &gt; 1.5u — piece may overshoot sensors
        </div>
      )}
      {d_sensor < 0.5 && (
        <div style={{ color: "#ff9800", marginTop: 4 }}>
          Sensor range ({d_sensor.toFixed(2)}u) very short — correction may be delayed
        </div>
      )}
    </div>
  );
}
