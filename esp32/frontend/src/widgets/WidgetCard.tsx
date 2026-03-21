import React from "react";
import { headingStyle } from "./shared";

interface WidgetCardProps {
  id: string;
  title: string;
  size: "half" | "full";
  onClose: (id: string) => void;
  onToggleSize: (id: string) => void;
  onDragStart: (e: React.DragEvent, id: string) => void;
  onDrop: (e: React.DragEvent, targetId: string) => void;
  children: React.ReactNode;
}

export function WidgetCard({ id, title, size, onClose, onToggleSize, onDragStart, onDrop, children }: WidgetCardProps) {
  return (
    <div
      draggable
      onDragStart={(e) => onDragStart(e, id)}
      onDragOver={(e) => e.preventDefault()}
      onDrop={(e) => onDrop(e, id)}
      style={{
        background: "#1a1a2e", 
        borderRadius: 10, 
        border: "1px solid #2a2a4a",
        display: "flex", 
        flexDirection: "column", 
        overflow: "hidden", 
        height: "100%",
        // This is the magic grid property. 
        // 1 / -1 spans the entire row. span 1 takes up a single column block.
        gridColumn: size === "full" ? "1 / -1" : "span 1",
        transition: "grid-column 0.2s ease" // Smooth snapping when resizing
      }}
    >
      <div style={{
        background: "#151525", padding: "8px 12px", borderBottom: "1px solid #2a2a4a",
        display: "flex", justifyContent: "space-between", alignItems: "center", cursor: "grab",
      }}>
        <h2 style={headingStyle}>{title}</h2>
        <div style={{ display: "flex", gap: 12, alignItems: "center" }}>
          {/* Resize Button */}
          <button 
            onClick={() => onToggleSize(id)}
            style={{ background: "transparent", border: "none", color: "#888", cursor: "pointer", padding: 0, fontSize: 14 }}
            title={size === "full" ? "Shrink Width" : "Expand Full Width"}
          >
            {size === "full" ? "◧" : "▭"}
          </button>
          {/* Close Button */}
          <button 
            onClick={() => onClose(id)}
            style={{ background: "transparent", border: "none", color: "#888", cursor: "pointer", padding: 0 }}
            title="Close Widget"
          >✕</button>
        </div>
      </div>
      <div style={{ padding: 16, flexGrow: 1 }}>{children}</div>
    </div>
  );
}