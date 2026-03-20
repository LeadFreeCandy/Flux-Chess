import { useState } from "react";
import { setRgb } from "../generated/api";
import { type WidgetProps, labelStyle, inputStyle, btnStyle } from "./shared";

export default function RgbWidget({ onStatus }: WidgetProps) {
  const [rgb, setRgbState] = useState({ r: 0, g: 0, b: 0 });

  const handleSetRgb = async () => {
    try {
      await setRgb(rgb);
      onStatus("RGB set");
    } catch (e) { onStatus(`Error: ${e}`); }
  };

  return (
    <>
      <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
        {["r", "g", "b"].map((color) => (
          <label key={color} style={labelStyle}>
            {color.toUpperCase()} <input type="number" value={(rgb as any)[color]} min={0} max={255}
              style={{ ...inputStyle, width: 55 }} onChange={(e) => setRgbState({ ...rgb, [color]: Number(e.target.value) })} />
          </label>
        ))}
        <button onClick={handleSetRgb} style={btnStyle}>Set</button>
      </div>
      <div style={{ marginTop: 16, height: 12, borderRadius: 6, border: "1px solid #333", background: `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})` }} />
    </>
  );
}