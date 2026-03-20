import React, { useState } from "react";
import { WIDGET_REGISTRY, type WidgetId, WidgetCard } from "./widgets";
import { btnStyle } from "./widgets/shared";

interface WidgetConfig {
  id: WidgetId;
  size: "half" | "full";
}

function App() {
  const [status, setStatus] = useState("Connected");
  
  // Track both the active widgets and their current size/layout state
  const [layout, setLayout] = useState<WidgetConfig[]>([
    { id: "surface", size: "half" },
    { id: "hexapawn", size: "half" },
    { id: "calibration", size: "half" },
    { id: "coils", size: "half" },
    { id: "rgb", size: "half" },
    { id: "serial", size: "full" }, // Serial defaults to full width on the bottom
  ]);

  const activeIds = layout.map(w => w.id);

  const toggleWidget = (id: WidgetId) => {
    setLayout(prev => {
      if (prev.some(w => w.id === id)) {
        return prev.filter(w => w.id !== id);
      }
      return [...prev, { id, size: "half" }]; // Default new widgets to half size
    });
  };

  const toggleSize = (id: WidgetId) => {
    setLayout(prev => prev.map(w => 
      w.id === id ? { ...w, size: w.size === "half" ? "full" : "half" } : w
    ));
  };

  const handleDragStart = (e: React.DragEvent, id: string) => {
    e.dataTransfer.setData("widgetId", id);
  };

  const handleDrop = (e: React.DragEvent, targetId: string) => {
    const draggedId = e.dataTransfer.getData("widgetId") as WidgetId;
    if (!draggedId || draggedId === targetId) return;

    setLayout(prev => {
      const newLayout = [...prev];
      const draggedIndex = newLayout.findIndex(w => w.id === draggedId);
      const targetIndex = newLayout.findIndex(w => w.id === targetId);
      
      const [draggedItem] = newLayout.splice(draggedIndex, 1);
      newLayout.splice(targetIndex, 0, draggedItem);
      return newLayout;
    });
  };

  return (
    <div style={{ fontFamily: "'SF Mono', 'Fira Code', monospace", background: "#111", color: "#e0e0e0", minHeight: "100vh", padding: "24px 32px" }}>
      <div style={{ maxWidth: 1200, margin: "0 auto" }}>
        
        {/* Top Header & Toggles */}
        <div style={{ 
          display: "flex", 
          flexWrap: "wrap", 
          alignItems: "center", 
          marginBottom: 16, 
          gap: 24 
        }}>
          <h1 style={{ margin: 0, fontSize: 24, color: "#fff" }}>FluxChess</h1>
          
          {/* Widget Toggles */}
          <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
            {(Object.keys(WIDGET_REGISTRY) as WidgetId[]).map(id => {
              const isActive = activeIds.includes(id);
              return (
                <button 
                  key={id} onClick={() => toggleWidget(id)}
                  style={{
                    ...btnStyle,
                    background: isActive ? "#1b5e20" : "#2a2a4a",
                    border: isActive ? "1px solid #2e7d32" : "1px solid #3a3a5a",
                    color: isActive ? "#e8f5e9" : "#888",
                    fontSize: 12, padding: "6px 12px", display: "flex", alignItems: "center", gap: 6
                  }}
                >
                  <span style={{ display: "inline-block", width: 8, height: 8, borderRadius: "50%", background: isActive ? "#4caf50" : "#555" }}/>
                  {WIDGET_REGISTRY[id].title}
                </button>
              )
            })}
          </div>
        </div>

        {/* Dedicated Full-Width Status Bar */}
        <div style={{ 
          marginBottom: 24, 
          padding: "10px 16px", 
          borderRadius: 8, 
          background: "#151525", 
          border: "1px solid #2a2a4a",
          borderLeft: "4px solid #4caf50", 
          display: "flex",
          alignItems: "center",
          gap: 12
        }}>
          <span style={{ fontSize: 12, fontWeight: "bold", color: "#888", textTransform: "uppercase", letterSpacing: 1 }}>
            System Status
          </span>
          <span style={{ fontSize: 14, color: "#e8f5e9", fontFamily: "monospace" }}>
            {status}
          </span>
        </div>

        {/* Dynamic Grid */}
        <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit, minmax(450px, 1fr))", gap: "16px", alignItems: "start" }}>
          {layout.map(config => {
            const WidgetComponent = WIDGET_REGISTRY[config.id].Component;
            return (
              <WidgetCard 
                key={config.id} 
                id={config.id} 
                title={WIDGET_REGISTRY[config.id].title} 
                size={config.size}
                onClose={toggleWidget as any} 
                onToggleSize={toggleSize as any}
                onDragStart={handleDragStart} 
                onDrop={handleDrop}
              >
                <WidgetComponent onStatus={setStatus} />
              </WidgetCard>
            );
          })}
        </div>
      </div>
    </div>
  );
}

export default App;