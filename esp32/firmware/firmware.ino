#include "board.h"
#include "serial_server.h"

Board board;
SerialServer serial(board);

// Allow board to process serial commands during long operations (hexapawn game)
void serialPollCallback() {
  serial.poll();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  board.setPollCallback(serialPollCallback);
  Serial.println("{\"type\":\"ready\"}");
}

void loop() {
  serial.poll();
}
