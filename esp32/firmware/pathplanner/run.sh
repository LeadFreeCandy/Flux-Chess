#!/bin/bash
set -e
cd "$(dirname "$0")"

BIN=pathplanner

echo "Building $BIN..."
g++ -std=c++17 -O2 -Wall -DPP_HOST_LOG -o "$BIN" main.cpp ../pathplanner.cpp

if [ -z "$1" ]; then
  echo "Usage: ./run.sh <problem.txt>   (searches ./ and ./tests/)"
  echo ""
  echo "Problem file format:"
  echo "  7 lines of 10 chars (top=y6, bottom=y0)"
  echo "  . = empty, P = white, p = black"
  echo "  blank line"
  echo "  fromX fromY toX toY"
  exit 1
fi

echo ""
if [ -f "$1" ]; then
  ./"$BIN" "$1"
elif [ -f "tests/$1" ]; then
  ./"$BIN" "tests/$1"
else
  echo "Not found: $1 (also checked tests/$1)"
  exit 1
fi
