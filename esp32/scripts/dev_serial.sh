#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL_PORT="/dev/cu.usbmodem101"

# ── Build Rust firmware ───────────────────────────────────────
echo "==> Building Rust firmware..."
export PATH="$HOME/.rustup/toolchains/esp/xtensa-esp-elf/esp-15.2.0_20250920/xtensa-esp-elf/bin:$PATH"
export LIBCLANG_PATH="$HOME/.rustup/toolchains/esp/xtensa-esp32-elf-clang/esp-20.1.1_20250829/esp-clang/lib"

cd "$ROOT/firmware-rs"
cargo build --release 2>&1 | tail -3

# ── Flash ─────────────────────────────────────────────────────
echo "==> Flashing..."
espflash flash --port "$SERIAL_PORT" \
  "$ROOT/firmware-rs/target/xtensa-esp32s3-none-elf/release/fluxchess-firmware"

echo "==> Waiting for ESP32 to boot..."
sleep 3

# ── Generate TypeScript bindings ──────────────────────────────
echo "==> Generating TypeScript bindings..."
cd "$ROOT/firmware-rs/api"
cargo +stable test --features ts --target aarch64-apple-darwin 2>&1 | tail -1

# ── Start frontend ────────────────────────────────────────────
echo "==> Starting frontend dev server..."
cd "$ROOT/frontend"
VITE_TRANSPORT=serial npx vite --open
