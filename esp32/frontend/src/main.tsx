import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { initTransport, needsUserGesture } from "./transport";
import App from "./App";

const root = createRoot(document.getElementById("root")!);

function renderApp() {
  root.render(
    <StrictMode>
      <App />
    </StrictMode>
  );
}

if (needsUserGesture()) {
  root.render(
    <div style={{ fontFamily: "monospace", padding: "40px", textAlign: "center" }}>
      <h1>FluxChess Dashboard</h1>
      <p>Serial mode — click to connect to ESP32</p>
      <button
        style={{ fontSize: "18px", padding: "12px 24px", cursor: "pointer" }}
        onClick={() => {
          initTransport()
            .then(renderApp)
            .catch((err) => {
              root.render(<div>Transport failed: {String(err)}</div>);
            });
        }}
      >
        Connect
      </button>
    </div>
  );
} else {
  initTransport().then(renderApp).catch((err) => {
    root.render(<div>Transport failed: {String(err)}</div>);
  });
}
