#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HASH_FILE="$ROOT/.source_hash"
SRC_DIR="$ROOT/esp_src"

CURRENT_HASH=$(cat "$SRC_DIR"/*.ino "$SRC_DIR"/*.h 2>/dev/null | shasum -a 256 | awk '{print $1}')
CACHED_HASH=$(cat "$HASH_FILE" 2>/dev/null || echo "")

if [ "$CURRENT_HASH" != "$CACHED_HASH" ]; then
  echo "Source changed, uploading..."
  "$SCRIPT_DIR/upload.sh"
  echo "$CURRENT_HASH" > "$HASH_FILE"
else
  echo "Source unchanged, skipping upload."
fi

"$SCRIPT_DIR/dashboard.sh"
