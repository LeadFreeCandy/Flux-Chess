#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$ROOT/.venv"

if [ ! -d "$VENV" ]; then
  echo "Creating venv..."
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q -r "$ROOT/requirements.txt"
fi

cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

"$VENV/bin/python" "$ROOT/webpages/server.py" &
SERVER_PID=$!
sleep 1

source "$ROOT/.env"
open -a "Google Chrome" "http://localhost:${HTTP_PORT}"

echo "Server running (PID $SERVER_PID). Press Ctrl+C to stop."
wait "$SERVER_PID"
