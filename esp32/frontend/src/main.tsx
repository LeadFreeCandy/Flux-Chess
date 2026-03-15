import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { initTransport, needsUserGesture } from "./transport";
import App from "./App";
import TestSurface from "./TestSurface";

const root = createRoot(document.getElementById("root")!);

// ?test=surface → render test page with mock data (no serial needed)
if (new URLSearchParams(window.location.search).get("test") === "surface") {
  root.render(
    <StrictMode>
      <TestSurface />
    </StrictMode>
  );
} else if (needsUserGesture()) {
  root.render(
    <div style={{ fontFamily: "monospace", padding: "40px", textAlign: "center", background: "#111", color: "#e0e0e0", minHeight: "100vh" }}>
      <h1>FluxChess Dashboard</h1>
      <p>Serial mode — click to connect to ESP32</p>
      <button
        style={{ fontSize: "18px", padding: "12px 24px", cursor: "pointer" }}
        onClick={() => {
          initTransport()
            .then(() => root.render(<StrictMode><App /></StrictMode>))
            .catch((err) => root.render(<div>Transport failed: {String(err)}</div>));
        }}
      >
        Connect
      </button>
    </div>
  );
} else {
  initTransport()
    .then(() => root.render(<StrictMode><App /></StrictMode>))
    .catch((err) => root.render(<div>Transport failed: {String(err)}</div>));
}
