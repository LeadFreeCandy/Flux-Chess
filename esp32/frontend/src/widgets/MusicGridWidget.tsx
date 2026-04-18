import React, { useState } from "react";
import { type WidgetProps, labelStyle, inputStyle, btnStyle } from "./shared";
// IMPORTANT: We assume you've added the API macro and regenerated api.ts!
// If your IDE complains about playNote, make sure you ran `python codegen/generate.py`.
import { playNote } from "../generated/api"; 

// A simple C major scale (4th octave)
const NOTES = [
  { name: "C4", freq: 262 },
  { name: "D4", freq: 294 },
  { name: "E4", freq: 330 },
  { name: "F4", freq: 349 },
  { name: "G4", freq: 392 },
  { name: "A4", freq: 440 },
  { name: "B4", freq: 494 },
  { name: "C5", freq: 523 },
];

export default function MusicGridWidget({ onStatus }: WidgetProps) {
  // Grid state: 3x3 array storing frequencies (null if unassigned)
  const [grid, setGrid] = useState<(number | null)[][]>([
    [null, null, null],
    [null, null, null],
    [null, null, null],
  ]);
  const [duration, setDuration] = useState(250); // Default note duration

  // The physical board spaces major grid squares by 3 units (SR_BLOCK)
  const colToX = (col: number) => col * 3;
  // Visual grid rows go top-to-bottom (0 to 2), physical goes bottom-to-top (0 to 6)
  const rowToY = (row: number) => (2 - row) * 3;

  // --- Drag & Drop Handlers ---
  const handleDragStart = (e: React.DragEvent, freq: number) => {
    e.dataTransfer.setData("text/plain", freq.toString());
  };

  const handleDrop = (e: React.DragEvent, row: number, col: number) => {
    e.preventDefault();
    const freqStr = e.dataTransfer.getData("text/plain");
    if (!freqStr) return;
    
    const freq = parseInt(freqStr, 10);
    const newGrid = [...grid];
    newGrid[row][col] = freq;
    setGrid(newGrid);
  };

  const handleDragOver = (e: React.DragEvent) => {
    e.preventDefault(); // Allows dropping
  };

  // --- Click Handler ---
  const handleSquareClick = async (row: number, col: number) => {
    const freq = grid[row][col];
    if (!freq) {
      onStatus("Square has no note assigned.");
      return;
    }

    const x = colToX(col);
    const y = rowToY(row);

    try {
      onStatus(`Playing ${freq}Hz at (${x},${y}) for ${duration}ms...`);
      // Call our newly generated API endpoint
      const res = await playNote({ x, y, freq_hz: freq, duration_ms: duration });
      if (!res.success) {
        onStatus(`Note failed: ${res.error}`);
      }
    } catch (e) {
      onStatus(`API Error: ${e}`);
    }
  };

  const clearGrid = () => {
    setGrid([
      [null, null, null],
      [null, null, null],
      [null, null, null],
    ]);
    onStatus("Cleared grid.");
  };

  return (
    <div style={{ display: "flex", height: "100%", gap: 16 }}>
      
      {/* --- Sidebar: Note Palette --- */}
      <div style={{ width: 150, display: "flex", flexDirection: "column", gap: 12 }}>
        <div>
          <label style={{ ...labelStyle, marginBottom: 4 }}>Duration (ms)</label>
          <input
            type="number"
            value={duration}
            onChange={(e) => setDuration(Number(e.target.value))}
            style={{ ...inputStyle, width: "100%" }}
            min={10}
            max={1000}
          />
        </div>
        
        <div style={{ flex: 1, overflowY: "auto", display: "flex", flexDirection: "column", gap: 6 }}>
          <label style={{ ...labelStyle, color: "#888" }}>Drag notes to grid</label>
          {NOTES.map((note) => (
            <div
              key={note.name}
              draggable
              onDragStart={(e) => handleDragStart(e, note.freq)}
              style={{
                background: "#1f77b4",
                color: "#fff",
                padding: "8px",
                borderRadius: "4px",
                textAlign: "center",
                cursor: "grab",
                fontWeight: "bold",
                userSelect: "none"
              }}
            >
              {note.name} ({note.freq}Hz)
            </div>
          ))}
        </div>

        <button onClick={clearGrid} style={{ ...btnStyle, background: "#d62728" }}>
          Clear Grid
        </button>
      </div>

      {/* --- Main Area: Hexapawn Grid --- */}
      <div style={{ flex: 1, display: "flex", justifyContent: "center", alignItems: "center", background: "#0a0a1a", borderRadius: 8 }}>
        <div style={{
          display: "grid",
          gridTemplateColumns: "repeat(3, 1fr)",
          gridTemplateRows: "repeat(3, 1fr)",
          gap: "8px",
          width: "100%",
          maxWidth: "400px",
          aspectRatio: "1/1",
          padding: "16px"
        }}>
          {grid.map((rowArr, row) =>
            rowArr.map((freq, col) => {
              const noteObj = NOTES.find(n => n.freq === freq);
              const label = noteObj ? noteObj.name : (freq ? `${freq}Hz` : "");
              
              return (
                <div
                  key={`${row}-${col}`}
                  onDrop={(e) => handleDrop(e, row, col)}
                  onDragOver={handleDragOver}
                  onClick={() => handleSquareClick(row, col)}
                  style={{
                    background: freq ? "#2ca02c" : "#1a1a2e",
                    border: freq ? "2px solid #4ade80" : "2px dashed #333",
                    borderRadius: "8px",
                    display: "flex",
                    flexDirection: "column",
                    justifyContent: "center",
                    alignItems: "center",
                    cursor: freq ? "pointer" : "default",
                    transition: "all 0.1s ease",
                  }}
                  onMouseEnter={(e) => {
                    if (freq) e.currentTarget.style.transform = "scale(0.95)";
                  }}
                  onMouseLeave={(e) => {
                    if (freq) e.currentTarget.style.transform = "scale(1)";
                  }}
                >
                  <span style={{ color: "#fff", fontSize: "24px", fontWeight: "bold" }}>
                    {label}
                  </span>
                  <span style={{ color: "#888", fontSize: "10px", marginTop: "4px" }}>
                    ({colToX(col)}, {rowToY(row)})
                  </span>
                </div>
              );
            })
          )}
        </div>
      </div>
    </div>
  );
}