#!/bin/bash
# Regenerate esp32/firmware/hexapawn_tables.h from the host-side BFS.
# Run whenever pathplanner.cpp, hexapawn.h, or the game rules change in a
# way that would alter precomputed plans.

set -e
cd "$(dirname "$0")"

echo "==> Building generator..."
g++ -std=c++17 -O2 -Wall -I../firmware \
    -o gen_hexapawn_tables \
    gen_hexapawn_tables.cpp ../firmware/pathplanner.cpp

echo "==> Generating hexapawn_tables.h..."
./gen_hexapawn_tables > ../firmware/hexapawn_tables.h

echo "==> Done."
ls -la ../firmware/hexapawn_tables.h
