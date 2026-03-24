import { useState, useRef, useCallback } from "react";
import { getBoardState, type GetBoardStateResponse } from "../generated/api";
import SurfacePlot from "../SurfacePlot";
import { type WidgetProps, btnStyle } from "./shared";

export default function SurfaceWidget({ onStatus }: WidgetProps) {
  const [boardState, setBoardState] = useState<GetBoardStateResponse | null>(null);
  const [polling, setPolling] = useState(false);
  const pollingRef = useRef(false);

  const pollBoardState = useCallback(async () => {
    if (!pollingRef.current) return;
    try {
      const res = await getBoardState();
      setBoardState(res);
    } catch (e) {
      onStatus(`Poll error: ${e}`);
    }
    if (pollingRef.current) setTimeout(pollBoardState, 100);
  }, [onStatus]);

  const togglePolling = () => {
    pollingRef.current = !polling;
    setPolling(!polling);
    if (!polling) pollBoardState();
  };

  return (
    <>
      <div style={{ marginBottom: 12 }}>
        <button onClick={togglePolling} style={{ ...btnStyle, background: polling ? "#b71c1c" : "#1565c0" }}>
          {polling ? "Stop Polling" : "Start 10Hz Poll"}
        </button>
      </div>
      {boardState ? (
        <SurfacePlot data={boardState.raw_strengths} />
      ) : (
        <div style={{ color: "#555", padding: 40, textAlign: "center" }}>Start polling to see sensor data</div>
      )}
    </>
  );
}