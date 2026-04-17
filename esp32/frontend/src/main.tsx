import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { initTransport, needsUserGesture } from "./transport";
import { installDiagnostics } from "./diagnostics";
import App from "./App";
import TestSurface from "./TestSurface";

installDiagnostics();

const root = createRoot(document.getElementById("root")!);

const urlParams = new URLSearchParams(window.location.search);

// Route: Just test the 3D surface animation
if (urlParams.get("test") === "surface") {
  root.render(
    <StrictMode>
      <TestSurface />
    </StrictMode>
  );
} 
// Route: Test the full widget dashboard layout without hardware
else if (urlParams.get("test") === "ui") {
  root.render(
    <StrictMode>
      <App />
    </StrictMode>
  );
} 
// Route: Standard Connection Screen
else if (needsUserGesture()) {
  root.render(
    <div style={{ 
      fontFamily: "'SF Mono', 'Fira Code', monospace", 
      padding: "80px 20px", 
      textAlign: "center", 
      background: "#111", 
      color: "#e0e0e0", 
      minHeight: "100vh" 
    }}>
      <h1 style={{ fontSize: 32, marginBottom: 8 }}>FluxChess Dashboard</h1>
      <p style={{ color: "#888", marginBottom: 32 }}>Select connection mode to proceed</p>
      
      <div style={{ display: "flex", gap: "16px", justifyContent: "center" }}>
        <button
          style={{ 
            fontSize: "15px", padding: "12px 24px", cursor: "pointer", 
            background: "#1565c0", color: "#fff", border: "none", 
            borderRadius: "6px", fontWeight: "bold"
          }}
          onClick={() => {
            initTransport()
              .then(() => root.render(<StrictMode><App /></StrictMode>))
              .catch((err) => root.render(<div style={{ color: "#ef5350", marginTop: 20 }}>Transport failed: {String(err)}</div>));
          }}
        >
          Connect to ESP32
        </button>
        
        <button
          style={{ 
            fontSize: "15px", padding: "12px 24px", cursor: "pointer", 
            background: "#2a2a4a", color: "#e0e0e0", border: "1px solid #3a3a5a", 
            borderRadius: "6px"
          }}
          onClick={() => {
            // Reload the page with the testing parameter
            window.location.search = "?test=ui";
          }}
        >
          Test UI Layout
        </button>
      </div>
    </div>
  );
} 
// Route: Auto-connect (if user gesture is already satisfied)
else {
  initTransport()
    .then(() => root.render(<StrictMode><App /></StrictMode>))
    .catch((err) => root.render(<div style={{ color: "#ef5350", padding: 20 }}>Transport failed: {String(err)}</div>));
}