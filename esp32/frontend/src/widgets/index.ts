import SurfaceWidget from "./SurfaceWidget";
import CoilsWidget from "./CoilsWidget";
import RgbWidget from "./RgbWidget";
import SerialWidget from "./SerialWidget";
import CalibrationWidget from "./CalibrationWidget";
import HexapawnWidget from "./HexapawnWidget";
import SpacedCoilsWidget from "./SpacedCoilsWidget";

export const WIDGET_REGISTRY = {
  surface: { title: "Magnetic Field", Component: SurfaceWidget },
  coils: { title: "Coil Grid", Component: CoilsWidget },
  calibration: { title: "Calibration", Component: CalibrationWidget },
  hexapawn: { title: "Hexapawn Game", Component: HexapawnWidget },
  rgb: { title: "RGB Underglow", Component: RgbWidget },
  serial: { title: "Serial Console", Component: SerialWidget },
  spaced_coils: { title: "Spaced Coil Grid", Component: SpacedCoilsWidget }
} as const;

export type WidgetId = keyof typeof WIDGET_REGISTRY;
export { WidgetCard } from "./WidgetCard";