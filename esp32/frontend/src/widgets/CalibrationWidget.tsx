import { useState } from "react";
import { type WidgetProps, btnStyle } from "./shared";

type CalibrationStep = "idle" | "calibrating_empty" | "instruct_piece" | "calibrating_piece";

export default function CalibrationWidget({ onStatus }: WidgetProps) {
  const [step, setStep] = useState<CalibrationStep>("idle");

  // Mock API call for empty board calibration
  const handleEmptyCalibrate = async () => {
    setStep("calibrating_empty");
    onStatus("Calibrating empty board...");
    
    // TODO: Replace with actual API call, e.g., await calibrateEmptyBoard();
    await new Promise((resolve) => setTimeout(resolve, 2000)); 
    
    setStep("idle");
    onStatus("Empty board calibration complete");
  };

  // Mock API call for piece calibration
  const handlePieceCalibrate = async () => {
    setStep("calibrating_piece");
    onStatus("Reading piece signature...");
    
    // TODO: Replace with actual API call, e.g., await calibratePiece({ x: 0, y: 0 });
    await new Promise((resolve) => setTimeout(resolve, 2000)); 
    
    setStep("idle");
    onStatus("Piece calibration complete");
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 16, height: "100%", justifyContent: "center" }}>
      
      {step === "idle" && (
        <>
          <div style={{ background: "#151525", padding: 16, borderRadius: 8, border: "1px solid #2a2a4a", textAlign: "center" }}>
            <h3 style={{ margin: "0 0 8px 0", fontSize: 14, color: "#e0e0e0" }}>Baseline Calibration</h3>
            <p style={{ margin: "0 0 16px 0", fontSize: 12, color: "#888" }}>
              Clear all magnetic pieces from the board to establish a baseline sensor reading.
            </p>
            <button onClick={handleEmptyCalibrate} style={{ ...btnStyle, width: "100%" }}>
              Calibrate Empty Board
            </button>
          </div>

          <div style={{ background: "#151525", padding: 16, borderRadius: 8, border: "1px solid #2a2a4a", textAlign: "center" }}>
            <h3 style={{ margin: "0 0 8px 0", fontSize: 14, color: "#e0e0e0" }}>Piece Calibration</h3>
            <p style={{ margin: "0 0 16px 0", fontSize: 12, color: "#888" }}>
              Measure the magnetic field strength of a specific piece to improve detection accuracy.
            </p>
            <button 
              onClick={() => setStep("instruct_piece")} 
              style={{ ...btnStyle, background: "#2a2a4a", border: "1px solid #3a3a5a", width: "100%" }}
            >
              Start Piece Calibration
            </button>
          </div>
        </>
      )}

      {step === "calibrating_empty" && (
        <div style={{ textAlign: "center", padding: 32 }}>
          <div style={{ color: "#4fc3f7", fontSize: 16, marginBottom: 8 }}>Reading sensors...</div>
          <div style={{ color: "#888", fontSize: 12 }}>Please keep magnetic objects away from the board.</div>
        </div>
      )}

      {step === "instruct_piece" && (
        <div style={{ background: "#1b5e20", padding: 16, borderRadius: 8, border: "1px solid #2e7d32", textAlign: "center" }}>
          <h3 style={{ margin: "0 0 8px 0", fontSize: 15, color: "#fff" }}>Action Required</h3>
          <p style={{ margin: "0 0 16px 0", fontSize: 13, color: "#a5d6a7" }}>
            Please place a magnetic piece exactly on the <strong>Bottom-Left corner (0, 0)</strong> of the board.
          </p>
          <div style={{ display: "flex", gap: 8 }}>
            <button onClick={() => setStep("idle")} style={{ ...btnStyle, background: "transparent", border: "1px solid #a5d6a7", color: "#a5d6a7", flex: 1 }}>
              Cancel
            </button>
            <button onClick={handlePieceCalibrate} style={{ ...btnStyle, background: "#fff", color: "#1b5e20", fontWeight: "bold", flex: 2 }}>
              Piece is Placed
            </button>
          </div>
        </div>
      )}

      {step === "calibrating_piece" && (
        <div style={{ textAlign: "center", padding: 32 }}>
          <div style={{ color: "#4fc3f7", fontSize: 16, marginBottom: 8 }}>Recording piece signature...</div>
          <div style={{ color: "#888", fontSize: 12 }}>Do not move the piece.</div>
        </div>
      )}

    </div>
  );
}