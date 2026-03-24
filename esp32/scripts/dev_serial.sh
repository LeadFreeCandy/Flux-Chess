#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
BUILD_DIR="$ROOT/build"
SERIAL_PORT="/dev/cu.usbmodem101"

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
