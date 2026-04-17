import SurfaceWidget from "./SurfaceWidget";
import CoilsWidget from "./CoilsWidget";
import RgbWidget from "./RgbWidget";
import SerialWidget from "./SerialWidget";
import CalibrationWidget from "./CalibrationWidget";
import HexapawnWidget from "./HexapawnWidget";
import SpacedCoilsWidget from "./SpacedCoilsWidget";
import MoveTestWidget from "./MoveTestWidget";
import PhysicsSettingsWidget from "./PhysicsSettingsWidget";
import DiagonalTestWidget from "./DiagonalTestWidget";
import EdgeMoveTestWidget from "./EdgeMoveTestWidget";
import CenterPieceWidget from "./CenterPieceWidget";

export const WIDGET_REGISTRY = {
  surface: { title: "Magnetic Field", Component: SurfaceWidget },
  coils: { title: "Coil Grid", Component: CoilsWidget },
  calibration: { title: "Calibration", Component: CalibrationWidget },
  hexapawn: { title: "Hexapawn Game", Component: HexapawnWidget },
  physics_settings: { title: "Physics Settings", Component: PhysicsSettingsWidget },
  move_test: { title: "Move Test", Component: MoveTestWidget },
  diagonal_test: { title: "Diagonal Test", Component: DiagonalTestWidget },
  edge_move: { title: "Edge Move Test", Component: EdgeMoveTestWidget },
  center_piece: { title: "Center Piece", Component: CenterPieceWidget },
  rgb: { title: "RGB Underglow", Component: RgbWidget },
  serial: { title: "Serial Console", Component: SerialWidget },
  spaced_coils: { title: "Spaced Coil Grid", Component: SpacedCoilsWidget }
} as const;

export type WidgetId = keyof typeof WIDGET_REGISTRY;
export { WidgetCard } from "./WidgetCard";
