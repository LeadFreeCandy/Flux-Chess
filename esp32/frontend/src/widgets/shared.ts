import React from "react";

export interface WidgetProps {
  onStatus: (msg: string) => void;
}

export const headingStyle: React.CSSProperties = {
  margin: 0, fontSize: 13, color: "#888", textTransform: "uppercase", letterSpacing: 1,
};

export const btnStyle: React.CSSProperties = {
  background: "#1565c0", color: "#fff", border: "none", borderRadius: 6,
  padding: "6px 14px", cursor: "pointer", fontSize: 13, fontFamily: "inherit",
};

export const inputStyle: React.CSSProperties = {
  background: "#0d0d1a", border: "1px solid #333", borderRadius: 4,
  color: "#e0e0e0", padding: "4px 6px", fontSize: 13, fontFamily: "inherit",
};

export const labelStyle: React.CSSProperties = {
  fontSize: 12, color: "#888", display: "flex", alignItems: "center", gap: 4,
};