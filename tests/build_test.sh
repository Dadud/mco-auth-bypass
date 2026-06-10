#!/usr/bin/env bash
# Build and run the test harness for the authlogin shim.
#
# Requires: mingw-w64 (provides i686-w64-mingw32-gcc)
#   Debian/Ubuntu:  sudo apt install gcc-mingw-w64
#   Fedora:         sudo dnf install mingw64-gcc
#   macOS:          brew install mingw-w64
set -euo pipefail

CC="${CC:-i686-w64-mingw32-gcc}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "error: $CC not found" >&2
  echo "       install mingw-w64 (Debian: apt install gcc-mingw-w64)" >&2
  exit 1
fi

cd "$(dirname "$0")"
echo "Building test_shim.exe..."
"$CC" -o test_shim.exe test_shim.c -static
echo "Running test_shim.exe..."
./test_shim.exe
