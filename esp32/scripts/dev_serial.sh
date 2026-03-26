#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
BUILD_DIR="$ROOT/build"
SERIAL_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$SERIAL_PORT" ]; then
  echo "ERROR: No ESP32 serial port found"
  exit 1
fi
echo "==> Using port: $SERIAL_PORT"

# Kill any existing Vite/node process from a previous run
pkill -f "vite" 2>/dev/null && echo "==> Killed previous Vite process." || true

echo "==> Running codegen..."
python3 "$ROOT/codegen/generate.py"

echo "==> Checking firmware..."
SRC_HASH=$(find "$ROOT/firmware" -name '*.h' -o -name '*.ino' -o -name '*.cpp' | \
  sort | xargs shasum | shasum | cut -d' ' -f1)
HASH_FILE="$BUILD_DIR/.src_hash"

if [ ! -f "$HASH_FILE" ] || [ "$SRC_HASH" != "$(cat "$HASH_FILE")" ]; then
  echo "==> Compiling firmware..."
  rm -rf "$BUILD_DIR"
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    "$ROOT/firmware"

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
    else
      echo "==> Port released."
    fi
  fi

  echo "==> Uploading firmware..."
  arduino-cli upload \
    --fqbn "$FQBN" \
    -p "$SERIAL_PORT" \
    --input-dir "$BUILD_DIR"

  echo "$SRC_HASH" > "$HASH_FILE"

  echo "==> Waiting for ESP32 to boot..."
  sleep 3
else
  echo "==> Firmware unchanged, skipping upload."
fi

echo "==> Starting frontend dev server..."
cd "$ROOT/frontend"
VITE_TRANSPORT=serial npx vite --open
