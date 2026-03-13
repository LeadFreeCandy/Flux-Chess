#!/usr/bin/env python3
import json
import os
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBPAGES_DIR = os.path.join(ROOT, "webpages")

def load_env():
    config = {}
    with open(os.path.join(ROOT, ".env")) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                config[k.strip()] = v.strip()
    return config

config = load_env()
SERIAL_PORT = config.get("SERIAL_PORT", "/dev/cu.usbmodem101")
BAUD_RATE = int(config.get("BAUD_RATE", "115200"))
HTTP_PORT = int(config.get("HTTP_PORT", "8765"))

latest_frame = {"type": "waiting", "values": [0] * 9}
frame_lock = threading.Lock()

def serial_reader():
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    print(f"Serial connected: {SERIAL_PORT} @ {BAUD_RATE}")
    while True:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                try:
                    parsed = json.loads(text)
                    with frame_lock:
                        global latest_frame
                        latest_frame = parsed
                except json.JSONDecodeError:
                    pass

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEBPAGES_DIR, **kwargs)

    def do_GET(self):
        if self.path == "/api/adc":
            with frame_lock:
                data = json.dumps(latest_frame).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            super().do_GET()

    def log_message(self, format, *args):
        pass

if __name__ == "__main__":
    threading.Thread(target=serial_reader, daemon=True).start()
    server = HTTPServer(("localhost", HTTP_PORT), Handler)
    print(f"http://localhost:{HTTP_PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
