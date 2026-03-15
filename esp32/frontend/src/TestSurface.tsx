import { useState, useEffect } from "react";
import SurfacePlot from "./SurfacePlot";

// Simulate realistic hall sensor data: 4 columns × 3 rows
// Values 0-4095 (12-bit ADC), baseline ~600-800, with a "peak" where a magnet is
function generateMockData(t: number): number[][] {
  const base = [
    [720, 680, 650],
    [700, 750, 630],
    [680, 710, 690],
    [660, 640, 670],
  ];

  return base.map((col, x) =>
    col.map((v, y) => {
      // Add a moving peak to simulate a magnet
      const peakX = 1.5 + Math.sin(t * 0.5) * 1.2;
      const peakY = 1.0 + Math.cos(t * 0.7) * 0.8;
      const dist = Math.sqrt((x - peakX) ** 2 + (y - peakY) ** 2);
      const peak = Math.max(0, 2000 * Math.exp(-dist * dist * 0.8));
      // Add some noise
      const noise = (Math.sin(t * 3 + x * 7 + y * 13) * 30);
      return Math.round(v + peak + noise);
    })
  );
}

export default function TestSurface() {
  const [data, setData] = useState(() => generateMockData(0));
  const [animating, setAnimating] = useState(true);

  useEffect(() => {
    if (!animating) return;
    let t = 0;
    const interval = setInterval(() => {
      t += 0.1;
      setData(generateMockData(t));
    }, 100);
    return () => clearInterval(interval);
  }, [animating]);

  return (
    <div style={{ background: "#111", padding: 24, minHeight: "100vh" }}>
      <h1 style={{ color: "#fff", fontFamily: "monospace" }}>Surface Plot Test</h1>
      <button
        onClick={() => setAnimating(!animating)}
        style={{ marginBottom: 16, padding: "8px 16px", fontSize: 14 }}
      >
        {animating ? "Stop" : "Start"} Animation
      </button>
      <div style={{ marginBottom: 16 }}>
        <h3 style={{ color: "#888", fontFamily: "monospace" }}>Raw data (4×3):</h3>
        <pre style={{ color: "#4fc3f7", fontFamily: "monospace", fontSize: 13 }}>
          {data.map((col, x) => `col${x}: [${col.join(", ")}]`).join("\n")}
        </pre>
      </div>
      <SurfacePlot data={data} />
    </div>
  );
}
