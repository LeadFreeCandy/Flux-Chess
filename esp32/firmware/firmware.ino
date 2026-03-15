#include "board.h"
#include "serial_server.h"

Board board;
SerialServer serial(board);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("{\"type\":\"ready\"}");
}

void loop() {
  serial.poll();
}
