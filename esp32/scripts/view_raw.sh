#!/bin/bash

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/.env"

arduino-cli monitor -p "$SERIAL_PORT" -c baudrate="$BAUD_RATE"
