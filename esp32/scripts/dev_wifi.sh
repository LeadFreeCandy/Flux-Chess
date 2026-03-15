#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/.env"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom"
BUILD_DIR="$ROOT/build"

echo "==> Running codegen..."
python3 "$ROOT/codegen/generate.py"

echo "==> Building frontend..."
cd "$ROOT/frontend"
VITE_TRANSPORT=http npm run build

echo "==> Preparing LittleFS data..."
rm -rf "$ROOT/data"
mkdir -p "$ROOT/data"

# Copy built assets (no gzip — LittleFS has plenty of space)
cp -r "$ROOT/frontend/dist"/* "$ROOT/data/"

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

echo "==> Building and uploading LittleFS image..."
# Build LittleFS image and flash to partition offset
mklittlefs -c "$ROOT/data" -s 0xAF0000 -p 256 -b 4096 "$BUILD_DIR/littlefs.bin"
esptool.py --chip esp32s3 --port "$SERIAL_PORT" --baud 921600 \
  write_flash 0x310000 "$BUILD_DIR/littlefs.bin"

echo "==> Opening dashboard..."
if command -v open &> /dev/null; then
  open "http://$ESP_IP"
elif command -v xdg-open &> /dev/null; then
  xdg-open "http://$ESP_IP"
fi

echo "==> Done! Dashboard at http://$ESP_IP"
