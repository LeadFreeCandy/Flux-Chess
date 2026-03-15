import { useState } from "react";
import {
  pulseCoil,
  getBoardState,
  setRgb,
  shutdown,
  type PulseCoilResponse,
  type GetBoardStateResponse,
} from "./generated/api";

function App() {
  const [boardState, setBoardState] = useState<GetBoardStateResponse | null>(
    null
  );
  const [pulseResult, setPulseResult] = useState<PulseCoilResponse | null>(
    null
  );
  const [pulseX, setPulseX] = useState(0);
  const [pulseY, setPulseY] = useState(0);
  const [pulseDuration, setPulseDuration] = useState(100);
  const [rgb, setRgbState] = useState({ r: 0, g: 0, b: 0 });
  const [status, setStatus] = useState("");

  const handleGetBoardState = async () => {
    try {
      const res = await getBoardState();
      setBoardState(res);
      setStatus("Board state updated");
    } catch (e) {
      setStatus(`Error: ${e}`);
    }
  };

  const handlePulseCoil = async () => {
    try {
      const res = await pulseCoil({
        x: pulseX,
        y: pulseY,
        duration_ms: pulseDuration,
      });
      setPulseResult(res);
      setStatus(res.success ? "Pulse sent" : `Pulse error: ${res.error}`);
    } catch (e) {
      setStatus(`Error: ${e}`);
    }
  };

  const handleSetRgb = async () => {
    try {
      await setRgb(rgb);
      setStatus("RGB set");
    } catch (e) {
      setStatus(`Error: ${e}`);
    }
  };

  const handleShutdown = async () => {
    try {
      await shutdown();
      setStatus("Shutdown sent");
    } catch (e) {
      setStatus(`Error: ${e}`);
    }
  };

  return (
    <div style={{ fontFamily: "monospace", padding: "20px", maxWidth: "800px" }}>
      <h1>FluxChess Dashboard</h1>
      <p>Transport: {__TRANSPORT__}</p>
      {status && <p><strong>{status}</strong></p>}

      <section>
        <h2>Board State</h2>
        <button onClick={handleGetBoardState}>Get Board State</button>
        {boardState && (
          <div>
            <h3>Raw Strengths</h3>
            <table style={{ borderCollapse: "collapse" }}>
              <tbody>
                {boardState.raw_strengths.map((col, x) => (
                  <tr key={x}>
                    {col.map((val, y) => (
                      <td
                        key={y}
                        style={{
                          border: "1px solid #ccc",
                          padding: "4px 8px",
                          textAlign: "center",
                          backgroundColor: `hsl(${(1 - val / 4095) * 240}, 80%, 70%)`,
                        }}
                      >
                        {val}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
            <p>Pieces detected: {boardState.piece_count}</p>
          </div>
        )}
      </section>

      <section>
        <h2>Pulse Coil</h2>
        <label>
          X: <input type="number" value={pulseX} min={0} max={9}
            onChange={(e) => setPulseX(Number(e.target.value))} />
        </label>{" "}
        <label>
          Y: <input type="number" value={pulseY} min={0} max={6}
            onChange={(e) => setPulseY(Number(e.target.value))} />
        </label>{" "}
        <label>
          Duration (ms): <input type="number" value={pulseDuration} min={1} max={1000}
            onChange={(e) => setPulseDuration(Number(e.target.value))} />
        </label>{" "}
        <button onClick={handlePulseCoil}>Pulse</button>
        {pulseResult && (
          <p>{pulseResult.success ? "OK" : `Error: ${pulseResult.error}`}</p>
        )}
      </section>

      <section>
        <h2>RGB Underglow</h2>
        <label>
          R: <input type="number" value={rgb.r} min={0} max={255}
            onChange={(e) => setRgbState({ ...rgb, r: Number(e.target.value) })} />
        </label>{" "}
        <label>
          G: <input type="number" value={rgb.g} min={0} max={255}
            onChange={(e) => setRgbState({ ...rgb, g: Number(e.target.value) })} />
        </label>{" "}
        <label>
          B: <input type="number" value={rgb.b} min={0} max={255}
            onChange={(e) => setRgbState({ ...rgb, b: Number(e.target.value) })} />
        </label>{" "}
        <button onClick={handleSetRgb}>Set</button>
      </section>

      <section>
        <h2>System</h2>
        <button onClick={handleShutdown} style={{ color: "red" }}>
          Shutdown ESP32
        </button>
      </section>
    </div>
  );
}

export default App;
