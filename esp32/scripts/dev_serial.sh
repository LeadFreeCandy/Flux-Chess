#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/.env"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom"
BUILD_DIR="$ROOT/build"

echo "==> Running codegen..."
python3 "$ROOT/codegen/generate.py"

echo "==> Checking firmware..."
SRC_HASH=$(find "$ROOT/firmware" -name '*.cpp' -o -name '*.h' -o -name '*.ino' | \
  sort | xargs shasum | shasum | cut -d' ' -f1)
HASH_FILE="$BUILD_DIR/.src_hash"

if [ ! -f "$HASH_FILE" ] || [ "$SRC_HASH" != "$(cat "$HASH_FILE")" ]; then
  echo "==> Compiling and uploading firmware..."
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    --build-property "build.extra_flags=-DWIFI_SSID=\"$WIFI_SSID\" -DWIFI_PASSWORD=\"$WIFI_PASSWORD\"" \
    "$ROOT/firmware"
  arduino-cli upload \
    --fqbn "$FQBN" \
    -p "$SERIAL_PORT" \
    --input-dir "$BUILD_DIR"
  echo "$SRC_HASH" > "$HASH_FILE"
else
  echo "==> Firmware unchanged, skipping upload."
fi

echo "==> Starting frontend dev server..."
cd "$ROOT/frontend"
VITE_TRANSPORT=serial npx vite --open
