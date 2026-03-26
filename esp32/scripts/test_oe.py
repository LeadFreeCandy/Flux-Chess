#!/usr/bin/env python3
"""Toggle OE pin by pulsing a coil with long duration, watch with multimeter/scope."""

import serial, time, json, os

PORT = os.environ.get("SERIAL_PORT", "/dev/cu.usbmodem101")
ser = serial.Serial(PORT, 115200, timeout=2)
time.sleep(4)

print(f"Connected to {PORT}")
print("This will pulse coil (0,0) for 1 second on, 1 second off.")
print("Measure GPIO 48 (OE) with a multimeter — should toggle LOW/HIGH.")
print("Ctrl+C to stop.\n")

try:
    cycle = 0
    while True:
        cycle += 1
        # Pulse coil at (0,0) for 1 second (1_000_000 us)
        cmd = json.dumps({"method": "pulse_coil", "params": {"x": 0, "y": 0, "duration_us": 1_000_000}})
        ser.write((cmd + "\n").encode())
        print(f"[{cycle}] OE LOW (coil on)...", end="", flush=True)

        # Wait for response (will take ~1 second since the pulse blocks)
        deadline = time.time() + 3
        while time.time() < deadline:
            if ser.in_waiting:
                line = ser.readline().decode(errors="replace").strip()
                try:
                    msg = json.loads(line)
                    if msg.get("method") == "pulse_coil":
                        status = msg.get("result", {})
                        print(f" done ({status})")
                        break
                except:
                    pass
            time.sleep(0.01)
        else:
            print(" TIMEOUT")

        print(f"[{cycle}] OE HIGH (coil off)...", flush=True)
        time.sleep(1)

except KeyboardInterrupt:
    print("\nStopped.")
finally:
    ser.close()
