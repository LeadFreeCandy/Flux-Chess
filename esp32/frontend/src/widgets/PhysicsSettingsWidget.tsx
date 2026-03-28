import { useState, useEffect } from "react";
import { setPhysicsParams as apiSetParams, getPhysicsParams as apiGetParams } from "../generated/api";
import { type WidgetProps, btnStyle, inputStyle, labelStyle } from "./shared";

const DEFAULT_PARAMS = {
  piece_mass_g: 2.7,
  max_current_a: 1.2,
  mu_static: 0.55,
  mu_kinetic: 0.45,
  target_velocity_mm_s: 300,
  target_accel_mm_s2: 1500,
  max_jerk_mm_s3: 20000,
  coast_friction_offset: 0,
  brake_pulse_ms: 100,
  pwm_freq_hz: 20000,
  pwm_compensation: 0.2,
  all_coils_equal: false,
  force_scale: 1.0,
  max_duration_ms: 5000,
};

const PARAM_LABELS: Record<string, string> = {
  piece_mass_g:         "mass (g)",
  max_current_a:        "max I (A)",
  mu_static:            "static \u03bc",
  mu_kinetic:           "kinetic \u03bc",
  target_velocity_mm_s: "target v (mm/s)",
  target_accel_mm_s2:   "target a (mm/s\u00B2)",
  max_jerk_mm_s3:       "max jerk (mm/s\u00B3)",
  coast_friction_offset: "coast fric offset",
  brake_pulse_ms:       "brake pulse (ms)",
  pwm_freq_hz:          "PWM freq (Hz)",
  pwm_compensation:     "PWM comp (0-1)",
  force_scale:          "force scale",
  max_duration_ms:      "timeout (ms)",
};

const STEP_MAP: Record<string, number> = {
  max_duration_ms: 100, pwm_freq_hz: 1000, brake_pulse_ms: 10,
  mu_static: 0.05, mu_kinetic: 0.05, pwm_compensation: 0.05,
  target_velocity_mm_s: 50, target_accel_mm_s2: 100, max_jerk_mm_s3: 1000,
  piece_mass_g: 0.1, max_current_a: 0.1, coast_friction_offset: 0.05,
  force_scale: 0.1,
};

export default function PhysicsSettingsWidget({ onStatus }: WidgetProps) {
  const [params, setParamsLocal] = useState(DEFAULT_PARAMS);
  const [saving, setSaving] = useState(false);
  const [dirty, setDirty] = useState(false);

  useEffect(() => {
    (async () => {
      try {
        const res = await apiGetParams() as any;
        if (res && typeof res === "object") setParamsLocal(p => ({ ...p, ...res }));
      } catch {}
    })();
  }, []);

  const updateParam = (key: string, val: string) => {
    setParamsLocal(p => ({ ...p, [key]: parseFloat(val) || 0 }));
    setDirty(true);
  };

  const save = async () => {
    setSaving(true);
    try {
      await apiSetParams(params as any);
      setDirty(false);
      onStatus("Physics params saved to ESP32");
    } catch (e) {
      onStatus(`Save failed: ${e}`);
    }
    setSaving(false);
  };

  const resetDefaults = async () => {
    setParamsLocal(DEFAULT_PARAMS);
    setSaving(true);
    try {
      await apiSetParams(DEFAULT_PARAMS as any);
      setDirty(false);
      onStatus("Physics params reset to defaults");
    } catch (e) {
      onStatus(`Reset failed: ${e}`);
    }
    setSaving(false);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8, height: "100%" }}>
      <div style={{
        display: "grid", gridTemplateColumns: "1fr 1fr", gap: "6px 12px",
        background: "#151525", padding: 10, borderRadius: 6, border: "1px solid #2a2a4a",
      }}>
        <label style={{ ...labelStyle, fontSize: 11, gridColumn: "1 / -1" }}>
          all coils equal
          <input type="checkbox" checked={!!params.all_coils_equal}
            onChange={e => { setParamsLocal(p => ({ ...p, all_coils_equal: e.target.checked })); setDirty(true); }}
          />
        </label>
        {Object.entries(PARAM_LABELS).map(([key, label]) => (
          <label key={key} style={{ ...labelStyle, fontSize: 11 }}>
            {label}
            <input
              type="number"
              step={STEP_MAP[key] ?? 0.1}
              value={(params as Record<string, number | boolean>)[key] as number}
              onChange={e => updateParam(key, e.target.value)}
              style={{ ...inputStyle, width: 70, fontSize: 11 }}
            />
          </label>
        ))}
      </div>

      <div style={{ display: "flex", gap: 8 }}>
        <button onClick={save} disabled={saving}
          style={{ ...btnStyle, flex: 1, background: dirty ? "#1b5e20" : "#2a2a4a", opacity: saving ? 0.5 : 1 }}>
          {saving ? "Saving..." : dirty ? "Save to ESP32" : "Saved"}
        </button>
        <button onClick={resetDefaults} disabled={saving}
          style={{ ...btnStyle, flex: 1, background: "#b71c1c", opacity: saving ? 0.5 : 1 }}>
          Reset Defaults
        </button>
      </div>
    </div>
  );
}
