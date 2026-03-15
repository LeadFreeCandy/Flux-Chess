#pragma once
#include "api.h"

ShutdownResponse handle_shutdown(const ShutdownRequest& req);
PulseCoilResponse handle_pulse_coil(const PulseCoilRequest& req);
GetBoardStateResponse handle_get_board_state(const GetBoardStateRequest& req);
SetRGBResponse handle_set_rgb(const SetRGBRequest& req);
