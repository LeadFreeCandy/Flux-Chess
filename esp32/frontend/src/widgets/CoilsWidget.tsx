import { useEffect, useRef, useState } from "react";
import { pulseCoil } from "../generated/api";
import { type WidgetProps, labelStyle, inputStyle, btnStyle } from "./shared";

// ── Board Constants ───────────────────────────────────────────
const GRID_COLS = 10;
const GRID_ROWS = 7;
const SR_BLOCK = 3;

// ── Layer Definitions (Mapped to lx, ly) ──────────────────────
const LAYERS = [
  { id: 1, name: "L1 (Base)", lx: 0, ly: 0, color: "#1f77b4" },
  { id: 2, name: "L2 (1/3 H)", lx: 1, ly: 0, color: "#ff7f0e" },
  { id: 3, name: "L3 (2/3 H)", lx: 2, ly: 0, color: "#2ca02c" },
  { id: 4, name: "L4 (1/3 V)", lx: 0, ly: 1, color: "#d62728" },
  { id: 5, name: "L5 (2/3 V)", lx: 0, ly: 2, color: "#9467bd" },
];

function getLayerInfo(x: number, y: number) {
  const lx = x % SR_BLOCK;
  const ly = y % SR_BLOCK;
  return LAYERS.find((l) => l.lx === lx && l.ly === ly) || null;
}

// ── Sequence Types ────────────────────────────────────────────
type SequenceItem = {
  id: string;
  type: "pulse" | "delay";
  x?: number;
  y?: number;
  duration: number; // ms
};

export default function CoilsWidget({ onStatus }: WidgetProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const wrapRef = useRef<HTMLDivElement>(null);

  // ── React State (UI Controls) ───────────────────────────────
  const [mode, setMode] = useState<"direct" | "sequence">("direct");
  const [pulseDuration, setPulseDuration] = useState(100);
  const [layerVis, setLayerVis] = useState<Record<number, boolean>>({
    1: true, 2: true, 3: true, 4: true, 5: true,
  });

  // Sequence State
  const [sequence, setSequence] = useState<SequenceItem[]>([]);
  const [isPlayingSeq, setIsPlayingSeq] = useState(false);
  const [draggedIdx, setDraggedIdx] = useState<number | null>(null);

  // ── Mutable State (Canvas Engine) ───────────────────────────
  const engine = useRef({
    hoveredX: -1, hoveredY: -1,
    pulsingX: -1, pulsingY: -1,
    scale: 1, offsetX: 0, offsetY: 0 
  }).current;

  // ── Canvas Engine & Render Loop ─────────────────────────────
  useEffect(() => {
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;
    const ctx = canvas.getContext("2d")!;

    const SQ_SIZE = 80;
    const RADIUS = SQ_SIZE / 2;
    const boardW = ((GRID_COLS - 1) * (SQ_SIZE / 3)) + SQ_SIZE;
    const boardH = ((GRID_ROWS - 1) * (SQ_SIZE / 3)) + SQ_SIZE;

    const draw = () => {
      const dpr = window.devicePixelRatio || 1;
      const wrapW = wrap.clientWidth;
      const wrapH = wrap.clientHeight;
      
      canvas.width = wrapW * dpr;
      canvas.height = wrapH * dpr;
      
      engine.scale = Math.min(wrapW / boardW, wrapH / boardH) * 0.85;
      engine.offsetX = (wrapW - (boardW * engine.scale)) / 2;
      engine.offsetY = (wrapH - (boardH * engine.scale)) / 2;

      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.save();
      ctx.scale(dpr, dpr);
      ctx.translate(engine.offsetX, engine.offsetY);
      ctx.scale(engine.scale, engine.scale);

      for (let i = LAYERS.length - 1; i >= 0; i--) {
        const layer = LAYERS[i];
        if (!layerVis[layer.id]) continue;

        for (let y = 0; y < GRID_ROWS; y++) {
          for (let x = 0; x < GRID_COLS; x++) {
            const coilLayer = getLayerInfo(x, y);
            if (!coilLayer || coilLayer.id !== layer.id) continue;

            const ry = GRID_ROWS - 1 - y; 
            const cx = (x * (SQ_SIZE / 3)) + RADIUS;
            const cy = (ry * (SQ_SIZE / 3)) + RADIUS;

            const isHovered = engine.hoveredX === x && engine.hoveredY === y;
            const isPulsing = engine.pulsingX === x && engine.pulsingY === y;

            ctx.beginPath();
            ctx.arc(cx, cy, RADIUS, 0, Math.PI * 2);

            ctx.fillStyle = layer.color;
            ctx.globalAlpha = isHovered && mode === "direct" ? 0.8 : (isHovered && mode === "sequence" ? 0.6 : 0.3);
            if (isPulsing) {
              ctx.fillStyle = "#ffca28";
              ctx.globalAlpha = 0.9;
            }
            ctx.fill();

            ctx.globalAlpha = 1.0;
            ctx.strokeStyle = isPulsing ? "#fff" : "rgba(255,255,255,0.2)";
            ctx.lineWidth = isHovered || isPulsing ? 3 : 1;
            ctx.stroke();

            ctx.beginPath();
            ctx.arc(cx, cy, RADIUS * 0.15, 0, Math.PI * 2);
            ctx.fillStyle = isPulsing ? "#fff" : layer.color;
            ctx.fill();
          }
        }
      }
      ctx.restore();
    };

    draw();

    // ── Input Handling ────────────────────────────────────────
    const screenToBoard = (sx: number, sy: number) => {
      const bx = (sx - engine.offsetX) / engine.scale;
      const by = (sy - engine.offsetY) / engine.scale;
      return { bx, by };
    };

    const getCoilAt = (bx: number, by: number) => {
      let closest = null;
      let minDist = Infinity;
      const MAX_SNAP_DIST = SQ_SIZE * 1.5; 

      for (let i = 0; i < LAYERS.length; i++) {
        const layer = LAYERS[i];
        if (!layerVis[layer.id]) continue;

        for (let y = 0; y < GRID_ROWS; y++) {
          for (let x = 0; x < GRID_COLS; x++) {
            const coilLayer = getLayerInfo(x, y);
            if (!coilLayer || coilLayer.id !== layer.id) continue;

            const ry = GRID_ROWS - 1 - y;
            const cx = (x * (SQ_SIZE / 3)) + RADIUS;
            const cy = (ry * (SQ_SIZE / 3)) + RADIUS;

            const dist = Math.sqrt(Math.pow(bx - cx, 2) + Math.pow(by - cy, 2));
            if (dist < minDist) {
              minDist = dist;
              closest = { x, y };
            }
          }
        }
      }
      return minDist <= MAX_SNAP_DIST ? closest : null;
    };

    const handleMouseDown = (e: MouseEvent) => {
      if (e.button === 0 && !isPlayingSeq) {
        const { bx, by } = screenToBoard(e.offsetX, e.offsetY);
        const hit = getCoilAt(bx, by);
        
        if (hit) {
          if (mode === "direct") {
            triggerPulse(hit.x, hit.y);
          } else if (mode === "sequence") {
            // Read latest pulse duration from the DOM directly to avoid strict stale closure
            const currentDuration = parseInt((document.getElementById("pulse-dur-input") as HTMLInputElement)?.value || "100", 10);
            
            setSequence(prev => [...prev, {
              id: Math.random().toString(36).substr(2, 9),
              type: "pulse",
              x: hit.x,
              y: hit.y,
              duration: currentDuration
            }]);
          }
        }
      }
    };

    const handleMouseMove = (e: MouseEvent) => {
      if (isPlayingSeq) return;
      
      const { bx, by } = screenToBoard(e.offsetX, e.offsetY);
      const hit = getCoilAt(bx, by);
      
      if (hit?.x !== engine.hoveredX || hit?.y !== engine.hoveredY) {
        engine.hoveredX = hit ? hit.x : -1;
        engine.hoveredY = hit ? hit.y : -1;
        canvas.style.cursor = hit ? (mode === "sequence" ? "copy" : "pointer") : "default";
        draw();
      }
    };

    const handleMouseLeave = () => {
      engine.hoveredX = -1;
      engine.hoveredY = -1;
      canvas.style.cursor = "default";
      draw();
    };

    canvas.addEventListener("mousedown", handleMouseDown);
    canvas.addEventListener("mousemove", handleMouseMove);
    canvas.addEventListener("mouseleave", handleMouseLeave);
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());

    const resizeObserver = new ResizeObserver(() => draw());
    resizeObserver.observe(wrap);

    return () => {
      canvas.removeEventListener("mousedown", handleMouseDown);
      canvas.removeEventListener("mousemove", handleMouseMove);
      canvas.removeEventListener("mouseleave", handleMouseLeave);
      resizeObserver.disconnect();
    };
  }, [layerVis, mode, isPlayingSeq]); // Re-bind on mode change so clicks route correctly

  // ── Hardware API Action ─────────────────────────────────────
  const triggerPulse = async (x: number, y: number, overrideDuration?: number) => {
    engine.pulsingX = x;
    engine.pulsingY = y;
    canvasRef.current?.dispatchEvent(new MouseEvent('mousemove')); 

    const dur = overrideDuration || pulseDuration;

    try {
      const res = await pulseCoil({ x, y, duration_ms: dur });
      if (mode === "direct") {
        onStatus(res.success ? `Pulsed Layer ${getLayerInfo(x, y)?.id} (${x},${y})` : `Error: ${res.error}`);
      }
    } catch (e) {
      onStatus(`Error: ${e}`);
    }

    engine.pulsingX = -1;
    engine.pulsingY = -1;
    canvasRef.current?.dispatchEvent(new MouseEvent('mousemove')); 
  };

  // ── Sequence Logic ──────────────────────────────────────────
  const playSequence = async () => {
    if (isPlayingSeq || sequence.length === 0) return;
    setIsPlayingSeq(true);
    onStatus(`Playing sequence (${sequence.length} steps)...`);

    for (const step of sequence) {
      if (step.type === "pulse") {
        await triggerPulse(step.x!, step.y!, step.duration);
      } else if (step.type === "delay") {
        await new Promise(r => setTimeout(r, step.duration));
      }
    }

    onStatus("Sequence complete.");
    setIsPlayingSeq(false);
  };

  const addDelay = () => {
    setSequence(prev => [...prev, {
      id: Math.random().toString(36).substr(2, 9),
      type: "delay",
      duration: 500
    }]);
  };

  const updateSeqItem = (id: string, newDuration: number) => {
    setSequence(prev => prev.map(item => item.id === id ? { ...item, duration: newDuration } : item));
  };

  const removeSeqItem = (id: string) => {
    setSequence(prev => prev.filter(item => item.id !== id));
  };

  // Drag and drop handlers
  const handleDragStart = (idx: number) => setDraggedIdx(idx);
  const handleDragOver = (e: React.DragEvent) => e.preventDefault();
  const handleDrop = (idx: number) => {
    if (draggedIdx === null || draggedIdx === idx) return;
    setSequence(prev => {
      const newSeq = [...prev];
      const [moved] = newSeq.splice(draggedIdx, 1);
      newSeq.splice(idx, 0, moved);
      return newSeq;
    });
    setDraggedIdx(null);
  };

  // ── Render ──────────────────────────────────────────────────
  return (
    <div style={{ display: "flex", height: "100%", overflow: "hidden" }}>
      
      {/* Sidebar Controls */}
      <div style={{ 
        width: 240, // Slightly wider for sequence UI
        background: "#16213e", 
        borderRight: "1px solid #2a2a4a", 
        padding: 12, 
        display: "flex", 
        flexDirection: "column", 
        gap: 16,
        overflowY: "auto" // Allow scrolling if sequence gets long
      }}>
        
        {/* Mode Toggle */}
        <div style={{ display: "flex", gap: 4, background: "#0f172a", padding: 4, borderRadius: 8 }}>
          <button 
            onClick={() => setMode("direct")} 
            style={{ ...btnStyle, flex: 1, padding: "6px 0", background: mode === "direct" ? "#1f77b4" : "transparent", color: mode === "direct" ? "#fff" : "#888" }}
          >Direct</button>
          <button 
            onClick={() => setMode("sequence")} 
            style={{ ...btnStyle, flex: 1, padding: "6px 0", background: mode === "sequence" ? "#9467bd" : "transparent", color: mode === "sequence" ? "#fff" : "#888" }}
          >Sequence</button>
        </div>

        {/* Global / Default Duration */}
        <div>
          <label style={{ ...labelStyle, marginBottom: 8 }}>Default Pulse (ms)</label>
          <input 
            id="pulse-dur-input"
            type="number" value={pulseDuration} min={1} max={1000} 
            style={{ ...inputStyle, width: "100%" }} 
            onChange={(e) => setPulseDuration(Number(e.target.value))} 
            disabled={isPlayingSeq}
          />
        </div>

        {/* Layer Toggles */}
        <div style={{ display: "flex", flexDirection: "column", gap: 8, paddingBottom: 12, borderBottom: "1px solid #2a2a4a" }}>
          <label style={{ ...labelStyle, marginBottom: 4, color: "#fff" }}>Visible Layers</label>
          {LAYERS.map((l) => (
            <label key={l.id} style={{ display: "flex", alignItems: "center", gap: 8, cursor: "pointer", fontSize: 11, color: "#e0e0e0" }}>
              <input 
                type="checkbox" 
                checked={layerVis[l.id]} 
                onChange={(e) => setLayerVis(prev => ({ ...prev, [l.id]: e.target.checked }))}
              />
              <div style={{ width: 12, height: 12, borderRadius: 2, background: l.color }} />
              {l.name}
            </label>
          ))}
        </div>

        {/* Sequence Builder UI */}
        {mode === "sequence" && (
          <div style={{ display: "flex", flexDirection: "column", flex: 1 }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
              <span style={{ fontSize: 12, color: "#fff", fontWeight: "bold" }}>Action List</span>
              <button onClick={addDelay} disabled={isPlayingSeq} style={{ ...btnStyle, background: "#4a4a6a", padding: "4px 8px", fontSize: 10 }}>+ Delay</button>
            </div>

            {/* Sequence List */}
            <div style={{ display: "flex", flexDirection: "column", gap: 4, flex: 1, marginBottom: 16 }}>
              {sequence.length === 0 && (
                <div style={{ fontSize: 11, color: "#666", textAlign: "center", padding: "20px 0", border: "1px dashed #333", borderRadius: 6 }}>
                  Click coils to add to sequence
                </div>
              )}
              {sequence.map((item, idx) => (
                <div 
                  key={item.id}
                  draggable={!isPlayingSeq}
                  onDragStart={() => handleDragStart(idx)}
                  onDragOver={handleDragOver}
                  onDrop={() => handleDrop(idx)}
                  style={{
                    display: "flex", alignItems: "center", gap: 8, background: "#0a0a1a", 
                    border: "1px solid #2a2a4a", borderRadius: 4, padding: "6px 8px",
                    opacity: draggedIdx === idx ? 0.5 : 1,
                    cursor: isPlayingSeq ? "default" : "grab"
                  }}
                >
                  <span style={{ color: "#555", cursor: "grab" }}>≡</span>
                  {item.type === "pulse" ? (
                    <div style={{ width: 10, height: 10, borderRadius: "50%", background: getLayerInfo(item.x!, item.y!)?.color || "#fff" }} />
                  ) : (
                    <span style={{ fontSize: 14, color: "#888" }}>⏱</span>
                  )}
                  
                  <div style={{ flex: 1, fontSize: 11, color: "#ccc" }}>
                    {item.type === "pulse" ? `Coil (${item.x},${item.y})` : "Wait"}
                  </div>

                  <input 
                    type="number" value={item.duration} step={50} min={1}
                    onChange={(e) => updateSeqItem(item.id, Number(e.target.value))}
                    disabled={isPlayingSeq}
                    style={{ ...inputStyle, width: 45, padding: "2px 4px", fontSize: 10 }}
                    title="Duration (ms)"
                  />
                  
                  <button 
                    onClick={() => removeSeqItem(item.id)}
                    disabled={isPlayingSeq}
                    style={{ background: "transparent", border: "none", color: "#d62728", cursor: isPlayingSeq ? "default" : "pointer" }}
                  >✕</button>
                </div>
              ))}
            </div>

            {/* Playback Controls */}
            <div style={{ display: "flex", gap: 4 }}>
              <button 
                onClick={playSequence} 
                disabled={isPlayingSeq || sequence.length === 0} 
                style={{ ...btnStyle, flex: 2, background: isPlayingSeq ? "#555" : "#2ca02c" }}
              >
                {isPlayingSeq ? "Playing..." : "▶ Play Sequence"}
              </button>
              <button 
                onClick={() => setSequence([])} 
                disabled={isPlayingSeq || sequence.length === 0} 
                style={{ ...btnStyle, flex: 1, background: "#d62728" }}
              >
                Clear
              </button>
            </div>
          </div>
        )}

        {/* Info text for Direct Mode */}
        {mode === "direct" && (
          <div style={{ marginTop: "auto", fontSize: 11, color: "#888", lineHeight: 1.4, background: "#111827", padding: 8, borderRadius: 6, border: "1px solid #1f2937" }}>
            <strong>Left-click</strong> a coil to trigger a test pulse immediately.
          </div>
        )}
      </div>

      {/* Canvas Wrap */}
      <div ref={wrapRef} style={{ flex: 1, position: "relative", background: "#0a0a1a", overflow: "hidden" }}>
        <canvas ref={canvasRef} style={{ position: "absolute", top: 0, left: 0, width: "100%", height: "100%", display: "block" }} />
      </div>

    </div>
  );
}