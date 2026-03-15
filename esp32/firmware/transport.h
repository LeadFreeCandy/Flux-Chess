#pragma once
#include <ESPAsyncWebServer.h>

void setup_http_routes(AsyncWebServer& server);
void handle_serial_command();
