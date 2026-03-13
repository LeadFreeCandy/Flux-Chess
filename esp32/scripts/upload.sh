#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/.env"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"

arduino-cli compile --fqbn "$FQBN" "$ROOT/esp_src"
arduino-cli upload --fqbn "$FQBN" -p "$SERIAL_PORT" "$ROOT/esp_src"
