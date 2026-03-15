#include "api_handlers.h"
#include "pins.h"

ShutdownResponse handle_shutdown(const ShutdownRequest& req) {
  ESP.restart();
  return {};
}

PulseCoilResponse handle_pulse_coil(const PulseCoilRequest& req) {
  PulseCoilResponse res = { false, PulseError::NONE };

  if (req.x >= GRID_COLS || req.y >= GRID_ROWS) {
    res.error = PulseError::INVALID_COIL;
    return res;
  }

  if (req.duration_ms > MAX_PULSE_MS) {
    res.error = PulseError::PULSE_TOO_LONG;
    return res;
  }

  // TODO: Map (x, y) to shift register bit and pulse it
  // For now, just validate and return success
  res.success = true;
  return res;
}

GetBoardStateResponse handle_get_board_state(const GetBoardStateRequest& req) {
  GetBoardStateResponse res = {};

  // Read raw ADC values from hall sensors
  for (int i = 0; i < NUM_HALL_SENSORS && i < GRID_COLS * GRID_ROWS; i++) {
    int col = i % GRID_COLS;
    int row = i / GRID_COLS;
    res.raw_strengths[col][row] = analogRead(HALL_PINS[i]);
  }

  // TODO: Piece detection from sensor data
  res.piece_count = 0;

  return res;
}

SetRGBResponse handle_set_rgb(const SetRGBRequest& req) {
  // TODO: Drive RGB LEDs when hardware is connected
  (void)req;
  return { true };
}
