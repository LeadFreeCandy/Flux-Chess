#!/usr/bin/env python3
"""Pretty-print hall sensor readings at 10Hz."""

import serial, time, json, os

PORT = os.environ.get("SERIAL_PORT", "/dev/cu.usbmodem101")
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=2)
time.sleep(3)  # wait for boot

print(f"Connected to {PORT}. Polling sensors at 10Hz. Ctrl+C to stop.\n")

try:
    while True:
        ser.write(b'{"method":"get_board_state","params":{}}\n')
        deadline = time.time() + 0.5
        while time.time() < deadline:
            if ser.in_waiting:
                line = ser.readline().decode(errors="replace").strip()
                try:
                    msg = json.loads(line)
                    if msg.get("method") == "get_board_state" and "result" in msg:
                        r = msg["result"]
                        grid = r.get("raw_sensor_values") or r.get("raw_strengths")
                        # Clear screen and print grid
                        print("\033[H\033[J", end="")
                        print("FluxChess Sensor Grid (4 cols × 3 rows)\n")
                        # Print header
                        print("       ", end="")
                        for col in range(len(grid)):
                            print(f"  col{col} ", end="")
                        print()
                        print("       " + "-------" * len(grid))
                        # Print rows (bottom to top so row 0 is at bottom)
                        for row in range(len(grid[0]) - 1, -1, -1):
                            print(f"row {row} |", end="")
                            for col in range(len(grid)):
                                val = grid[col][row]
                                print(f"  {val:4d} ", end="")
                            print("|")
                        print("       " + "-------" * len(grid))
                        print(f"\n{time.strftime('%H:%M:%S')}", end="", flush=True)
                        break
                except (json.JSONDecodeError, KeyError):
                    pass
            time.sleep(0.01)
        time.sleep(0.1)
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    ser.close()
