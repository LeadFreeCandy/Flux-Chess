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
  force_k: 5.0,
  force_epsilon: 0.5,
  falloff_exp: 2.0,
  voltage_scale: 1.0,
  friction_static: 1.0,
  friction_kinetic: 3.0,
  target_velocity: 3.0,
  target_accel: 10.0,
  sensor_k: 500.0,
  sensor_falloff: 2.0,
  sensor_threshold: 50.0,
  max_duration_ms: 5000,
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
          {(Object.keys(DEFAULT_PHYSICS_PARAMS) as (keyof typeof DEFAULT_PHYSICS_PARAMS)[]).map(key => (
            <label key={key} style={labelStyle}>
              {key.replace(/_/g, " ")}
              <input
                type="number"
                step={key === 'max_duration_ms' ? 1 : 0.1}
                value={physicsParams[key]}
                onChange={e => updateParam(key, e.target.value)}
                style={{ ...inputStyle, width: 70 }}
              />
            </label>
          ))}
        </div>
      )}

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
