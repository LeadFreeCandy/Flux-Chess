#!/bin/bash
set -e

# Build and upload the demo firmware, then monitor serial output.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
BUILD_DIR="$ROOT/build_demo"
DEMO_DIR="$ROOT/firmware_demo"
FW_DIR="$ROOT/firmware"
SERIAL_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$SERIAL_PORT" ]; then
  echo "ERROR: No ESP32 serial port found"
  exit 1
fi
echo "==> Using port: $SERIAL_PORT"

echo "==> Running codegen..."
python3 "$ROOT/codegen/generate.py"

# Symlink all firmware headers into demo sketch dir so includes resolve
echo "==> Linking firmware headers..."
for h in "$FW_DIR"/*.h; do
  ln -sf "$h" "$DEMO_DIR/$(basename "$h")"
done
# Also link the pathplanner subdirectory if it exists
if [ -d "$FW_DIR/pathplanner" ]; then
  ln -sfn "$FW_DIR/pathplanner" "$DEMO_DIR/pathplanner"
fi

echo "==> Compiling demo firmware..."
rm -rf "$BUILD_DIR"
arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_DIR" \
  "$DEMO_DIR"

# Release serial port by reloading the Chrome tab holding it
if lsof "$SERIAL_PORT" > /dev/null 2>&1; then
  echo "==> Serial port busy, reloading Chrome tab to release..."
  osascript -e '
    tell application "Google Chrome"
      repeat with w in windows
        repeat with t in tabs of w
          if URL of t contains "localhost" or URL of t contains "FluxChess" then
            tell t to reload
          end if
        end repeat
      end repeat
    end tell
  ' 2>/dev/null || true
  sleep 2
  if lsof "$SERIAL_PORT" > /dev/null 2>&1; then
    echo "⚠  Port still busy after reload."
    read -p "Kill the process holding the port? [y/N] " KILL_PORT
    if [[ "$KILL_PORT" =~ ^[Yy]$ ]]; then
      lsof -t "$SERIAL_PORT" | xargs kill -9 2>/dev/null
      sleep 1
      echo "==> Process killed."
    fi
  fi
fi

echo "==> Uploading demo firmware..."
arduino-cli upload \
  --fqbn "$FQBN" \
  -p "$SERIAL_PORT" \
  --input-dir "$BUILD_DIR"

echo "==> Waiting for ESP32 to boot..."
sleep 3

echo "==> Monitoring serial output (Ctrl+C to stop)..."
arduino-cli monitor -p "$SERIAL_PORT" --config baudrate=115200
