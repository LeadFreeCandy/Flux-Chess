import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { initTransport } from "./transport";
import App from "./App";

const root = createRoot(document.getElementById("root")!);

initTransport()
  .then(() => {
    root.render(
      <StrictMode>
        <App />
      </StrictMode>
    );
  })
  .catch((err) => {
    root.render(<div>Transport failed: {String(err)}</div>);
  });
