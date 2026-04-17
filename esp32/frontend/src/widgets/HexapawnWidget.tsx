import { useState, useRef } from "react";
import { getBoardState } from "../generated/api";
import { transport } from "../transport/index";
import { onSerialLog, type SerialLogEntry } from "../transport";
import { type WidgetProps, btnStyle } from "./shared";

const PIECE_NONE = 0;
const PIECE_WHITE = 1;
const PIECE_BLACK = 2;

const MAJOR_COLS = [0, 3, 6];
const MAJOR_ROWS = [0, 3, 6];
const GRAVE_COL = 9;
const ALL_COLS = [...MAJOR_COLS, GRAVE_COL];

type Step = "idle" | "playing" | "done";

const DEFAULT_HINT_PULSE_MS = 50;
const DEFAULT_HINT_INTERVAL_MS = 1000;

export default function HexapawnWidget({ onStatus }: WidgetProps) {
  const [step, setStep] = useState<Step>("idle");
  const [logs, setLogs] = useState<string[]>([]);
  const [result, setResult] = useState<string | null>(null);
  const [hintPulseMs, setHintPulseMs] = useState(DEFAULT_HINT_PULSE_MS);
  const [hintIntervalMs, setHintIntervalMs] = useState(DEFAULT_HINT_INTERVAL_MS);
  const [pieces, setPieces] = useState<number[][] | null>(null);
  const logsRef = useRef<string[]>([]);
  const logBoxRef = useRef<HTMLDivElement>(null);

  const [resetting, setResetting] = useState(false);

  const fetchBoard = async () => {
    try {
      const res = await getBoardState();
      setPieces(res.pieces);
    } catch {}
  };

  const resetBoardPhysical = async () => {
    if (resetting) return;
    setResetting(true);
    onStatus("Resetting board...");
    try {
      const res = await transport.call("reset_board", {}, { timeoutMs: 180000 }) as {
        success: boolean; moves?: number; error?: string;
      };
      if (res.success) {
        onStatus(res.moves === 0 ? "Board already reset" : `Board reset (${res.moves} moves)`);
      } else {
        onStatus(`Reset failed: ${res.error ?? "unknown"}`);
      }
    } catch (e) {
      onStatus(`Reset error: ${e}`);
    }
    await fetchBoard();
    setResetting(false);
  };

  const getPiece = (x: number, y: number): number => {
    if (!pieces || x >= pieces.length || y >= pieces[0].length) return PIECE_NONE;
    return pieces[x][y];
  };

  const startGame = async () => {
    setStep("playing");
    setResult(null);
    logsRef.current = [];
    setLogs([]);
    onStatus("Game starting...");

    // hexapawn_play is a long-running command (can run for minutes). We send
    // it fire-and-forget so it does NOT occupy the transport's pending slot —
    // that lets other short commands (get_board_state, etc.) continue to work
    // during the game. Game completion is detected from LOG_BOARD messages.

    let finished = false;
    const finish = (resultText: string, statusText: string) => {
      if (finished) return;
      finished = true;
      setResult(resultText);
      onStatus(statusText);
      unsub();
      fetchBoard().finally(() => setStep("done"));
    };

    const unsub = onSerialLog((entry: SerialLogEntry) => {
      if (entry.dir !== "rx") return;
      try {
        const msg = JSON.parse(entry.data);
        if (msg.type !== "log" || typeof msg.msg !== "string") return;
        const line = msg.msg;
        if (!line.startsWith("hexapawn:")) return;

        logsRef.current = [...logsRef.current, line];
        setLogs([...logsRef.current]);
        if (logBoxRef.current) {
          logBoxRef.current.scrollTop = logBoxRef.current.scrollHeight;
        }

        // Status updates from key log lines
        if (line.includes("your turn")) onStatus("Your turn — lift a white piece");
        else if (line.includes("AI thinking")) onStatus("AI thinking...");
        else if (line.includes("piece lifted")) onStatus("Piece lifted — place on valid square");
        else if (line.includes("piece placed")) onStatus("Piece placed");

        // Terminal states — end the game locally
        if (line.includes("WINS")) {
          const winner = line.includes("White") ? "white" : "black";
          finish(winner === "white" ? "You win!" : "AI wins!",
                 winner === "white" ? "You win!" : "AI wins!");
        } else if (line.includes("ERROR")) {
          finish(`Error: ${line.replace("hexapawn: ", "")}`, "Game error");
        } else if (line.includes("timeout waiting")) {
          finish("Timeout", "Game timed out");
        } else if (line.includes("already running")) {
          finish("Already running on device", "Game already running");
        }
      } catch {}
    });

    try {
      await transport.send("hexapawn_play", {
        hint_pulse_ms: hintPulseMs,
        hint_interval_ms: hintIntervalMs,
      });
    } catch (e) {
      finish(`Error: ${e}`, `Game error: ${e}`);
    }
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", alignItems: "center", gap: 8 }}>

      {/* Board */}
      <div style={{
        display: "grid",
        gridTemplateColumns: `repeat(4, 70px)`,
        gridTemplateRows: `repeat(3, 70px)`,
        gap: 4, background: "#222", padding: 6, borderRadius: 8,
      }}>
        {MAJOR_ROWS.slice().reverse().map((gy) =>
          ALL_COLS.map((gx) => {
            const piece = getPiece(gx, gy);
            const isGrave = gx === GRAVE_COL;
            const isDark = ((gx / 3) + (gy / 3)) % 2 === 1;

            return (
              <div
                key={`${gx}-${gy}`}
                style={{
                  width: 70, height: 70,
                  background: isGrave ? "#1a1a2e" : isDark ? "#3a5a7c" : "#D7BA89",
                  border: isGrave ? "2px dashed #333" : "none",
                  display: "flex", justifyContent: "center", alignItems: "center",
                  fontSize: 44,
                  color: piece === PIECE_BLACK ? "#333" : "#fff",
                  WebkitTextStroke: piece !== PIECE_NONE ? "1px #888" : undefined,
                  userSelect: "none", borderRadius: 4,
                  opacity: isGrave ? 0.7 : 1,
                }}
              >
                {piece === PIECE_WHITE ? "\u265F" : piece === PIECE_BLACK ? "\u265F" : ""}
              </div>
            );
          })
        )}
      </div>

      {/* Controls */}
      <div style={{ display: "flex", gap: 8, width: "100%", maxWidth: 400, alignItems: "center" }}>
        {step === "idle" && (
          <>
            <button onClick={startGame} style={{ ...btnStyle, flex: 1, background: "#1b5e20", fontSize: 14, padding: 10 }}>
              Play
            </button>
            <label style={{ fontSize: 10, color: "#888", display: "flex", alignItems: "center", gap: 4 }}>
              hint ms
              <input type="number" value={hintPulseMs} step={10} min={0} max={500}
                onChange={e => setHintPulseMs(parseInt(e.target.value) || 0)}
                style={{ width: 45, background: "#0d0d1a", border: "1px solid #333", borderRadius: 4, color: "#e0e0e0", padding: "2px 4px", fontSize: 11 }} />
            </label>
            <label style={{ fontSize: 10, color: "#888", display: "flex", alignItems: "center", gap: 4 }}>
              every ms
              <input type="number" value={hintIntervalMs} step={100} min={0} max={5000}
                onChange={e => setHintIntervalMs(parseInt(e.target.value) || 0)}
                style={{ width: 55, background: "#0d0d1a", border: "1px solid #333", borderRadius: 4, color: "#e0e0e0", padding: "2px 4px", fontSize: 11 }} />
            </label>
          </>
        )}
        {step === "playing" && (
          <div style={{ flex: 1, textAlign: "center", color: "#4fc3f7", fontSize: 14 }}>
            Game in progress...
          </div>
        )}
        {step === "done" && (
          <>
            <div style={{
              flex: 2, textAlign: "center", padding: 8, borderRadius: 4, fontWeight: "bold",
              background: result?.includes("You") ? "#1b5e20" : "#b71c1c", color: "#fff",
            }}>
              {result}
            </div>
            <button onClick={() => { setStep("idle"); setLogs([]); setResult(null); }}
              style={{ ...btnStyle, flex: 1, background: "#2a2a4a", border: "1px solid #3a3a5a" }}>
              Reset
            </button>
          </>
        )}
        <button onClick={fetchBoard} style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a" }}>
          Refresh
        </button>
        <button onClick={resetBoardPhysical} disabled={resetting || step === "playing"}
          style={{
            ...btnStyle,
            background: resetting ? "#444" : "#b45309",
            border: "1px solid #78350f",
            opacity: (resetting || step === "playing") ? 0.5 : 1,
          }}>
          {resetting ? "Resetting..." : "Reset Board"}
        </button>
      </div>

      {/* Game log */}
      {logs.length > 0 && (
        <div ref={logBoxRef} style={{
          width: "100%", maxWidth: 400, flex: 1, minHeight: 100, maxHeight: 200,
          overflow: "auto", background: "#0a0a1a", border: "1px solid #2a2a4a",
          borderRadius: 6, padding: 8, fontSize: 11, fontFamily: "monospace",
          color: "#8f8", userSelect: "text", cursor: "text",
        }}>
          {logs.map((line, i) => (
            <div key={i} style={{
              color: line.includes("WINS") ? "#f57f17"
                : line.includes("your turn") ? "#4caf50"
                : line.includes("AI") ? "#ef5350"
                : line.includes("ERROR") ? "#ef5350"
                : "#8f8"
            }}>
              {line.replace("hexapawn: ", "")}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
