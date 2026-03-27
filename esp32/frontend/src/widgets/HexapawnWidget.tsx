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
  piece_mass_g: 4.3,
  max_current_a: 1.0,
  mu_static: 0.35,
  mu_kinetic: 0.25,
  target_velocity_mm_s: 100,
  target_accel_mm_s2: 500,
  max_jerk_mm_s3: 50000,
  active_brake: true,
  pwm_freq_hz: 20000,
  pwm_compensation: 0.2,
  all_coils_equal: false,
  force_scale: 1.0,
  max_duration_ms: 5000,
};

const PARAM_INFO: Record<keyof typeof DEFAULT_PHYSICS_PARAMS, string> = {
  piece_mass_g:          "mass (g)",
  max_current_a:         "max current (A)",
  mu_static:             "static mu",
  mu_kinetic:            "kinetic mu",
  target_velocity_mm_s:  "target v (mm/s)",
  target_accel_mm_s2:    "target a (mm/s\u00B2)",
  max_jerk_mm_s3:        "max jerk (mm/s\u00B3)",
  pwm_freq_hz:           "PWM freq (Hz)",
  pwm_compensation:      "PWM comp (0-1)",
  force_scale:           "force scale",
  max_duration_ms:       "timeout (ms)",
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
  const [physicsParams, setPhysicsParamsRaw] = useState(() => {
    try {
      const saved = localStorage.getItem('fluxchess_physics_params');
      if (saved) return { ...DEFAULT_PHYSICS_PARAMS, ...JSON.parse(saved) };
    } catch {}
    return DEFAULT_PHYSICS_PARAMS;
  });

  const setPhysicsParams = (val: typeof DEFAULT_PHYSICS_PARAMS | ((prev: typeof DEFAULT_PHYSICS_PARAMS) => typeof DEFAULT_PHYSICS_PARAMS)) => {
    setPhysicsParamsRaw((prev: typeof DEFAULT_PHYSICS_PARAMS) => {
      const next = typeof val === "function" ? val(prev) : val;
      localStorage.setItem('fluxchess_physics_params', JSON.stringify(next));
      return next;
    });
  };
  const [simPos, setSimPos] = useState<{ x: number; y: number } | null>(null);

  // Listen for simulated position updates from MoveTestWidget
  useEffect(() => {
    const handler = (e: Event) => {
      const detail = (e as CustomEvent).detail;
      setSimPos(detail ? { x: detail.x, y: detail.y } : null);
    };
    window.addEventListener("fluxchess-sim-pos", handler);
    return () => window.removeEventListener("fluxchess-sim-pos", handler);
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
          <label style={labelStyle}>
            active brake
            <input type="checkbox" checked={!!physicsParams.active_brake}
              onChange={e => setPhysicsParams(p => ({ ...p, active_brake: e.target.checked }))} />
          </label>
          <label style={labelStyle}>
            all coils equal
            <input type="checkbox" checked={!!physicsParams.all_coils_equal}
              onChange={e => setPhysicsParams(p => ({ ...p, all_coils_equal: e.target.checked }))} />
          </label>
          {(Object.entries(PARAM_INFO) as [keyof typeof PARAM_INFO, string][]).map(([key, label]) => (
            <label key={key} style={labelStyle}>
              {label}
              <input
                type="number"
                step={key === 'max_duration_ms' || key === 'pwm_freq_hz' ? 1000 : (key === 'mu_static' || key === 'mu_kinetic' || key === 'pwm_compensation') ? 0.05 : (key === 'target_velocity_mm_s' || key === 'target_accel_mm_s2') ? 1 : 0.1}
                value={physicsParams[key] as number}
                onChange={e => updateParam(key, e.target.value)}
                style={{ ...inputStyle, width: 70 }}
              />
            </label>
          ))}
          <button onClick={() => setPhysicsParams(DEFAULT_PHYSICS_PARAMS)}
            style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", fontSize: 10, gridColumn: "1 / -1" }}>
            Reset Defaults
          </button>
        </div>
      )}

      {usePhysics && showParams && <PhysicsDebug p={physicsParams} />}

      <div style={{ position: "relative" }}>
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
      {simPos && (() => {
        // Map continuous grid coords to pixel position on the board
        // Grid: 4 columns (0,3,6,9) mapped to 0-3, each cell is 70px + 4px gap
        // padding: 6px on each side
        const colIdx = simPos.x / 3;  // 0-3 for cols 0,3,6,9
        const rowIdx = 2 - simPos.y / 3;  // inverted, 0=top
        const cellSize = 70;
        const gap = 4;
        const pad = 6;
        const px = pad + colIdx * (cellSize + gap) + cellSize / 2;
        const py = pad + rowIdx * (cellSize + gap) + cellSize / 2;
        return (
          <div style={{
            position: "absolute", left: px - 8, top: py - 8,
            width: 16, height: 16, borderRadius: "50%",
            background: "rgba(255, 50, 50, 0.8)",
            border: "2px solid #fff",
            pointerEvents: "none",
            transition: "left 0.05s, top 0.05s",
            zIndex: 10,
          }} />
        );
      })()}
      </div>
    </div>
  );
}

function PhysicsDebug({ p }: { p: typeof DEFAULT_PHYSICS_PARAMS }) {
  const { piece_mass_g, max_current_a, mu_static, mu_kinetic,
          target_velocity_mm_s: tv, target_accel_mm_s2: ta } = p;

  const weight = piece_mass_g * 9.81;  // mN
  const peak_fx = 54.0 * max_current_a;  // approx from force table L0
  const peak_fz = 101.0 * max_current_a;
  const normal_active = weight + peak_fz;
  const static_fric = mu_static * normal_active;
  const kinetic_fric = mu_kinetic * weight;  // coil off during coast
  const t_cross = 9 * 12.667 / tv;  // 9 grid units in mm
  const coast_decel = kinetic_fric / (piece_mass_g * 1e-3);
  const stop_dist = (tv * tv) / (2 * coast_decel);

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
      <div style={{ color: "#666", fontSize: 10, marginBottom: 4 }}>REAL-UNIT PHYSICS</div>
      {row("Piece weight", `${weight.toFixed(1)} mN`)}
      {row("Peak lateral (L0, 1A)", `${(54.0 * max_current_a).toFixed(1)} mN`)}
      {row("Peak Fz (L0, 1A)", `${peak_fz.toFixed(1)} mN (downward)`)}
      {row("Normal force (coil on)", `${normal_active.toFixed(1)} mN`)}
      {row("Static friction (coil on)", `${static_fric.toFixed(1)} mN`)}
      {row("Can overcome stiction?", peak_fx > static_fric ? `YES (${(peak_fx/static_fric).toFixed(1)}x)` : "NO")}
      {row("Coast friction (coil off)", `${kinetic_fric.toFixed(1)} mN`)}
      {row("Coast decel", `${coast_decel.toFixed(0)} mm/s\u00B2`)}
      {row("Stopping distance", `${stop_dist.toFixed(1)} mm at ${tv} mm/s`)}
      {row("Board traverse (114mm)", `${t_cross.toFixed(2)}s`)}
      {peak_fx < static_fric && (
        <div style={{ color: "#ef5350", marginTop: 4 }}>
          Peak force ({peak_fx.toFixed(1)}) &lt; static friction ({static_fric.toFixed(1)}) — piece won't move!
        </div>
      )}
    </div>
  );
}
