#!/usr/bin/env bash
# Build script for authlogin.dll (cross-compile from Linux/macOS to Windows)
#
# Requires: mingw-w64 (provides i686-w64-mingw32-gcc)
#   Debian/Ubuntu:  sudo apt install gcc-mingw-w64
#   Fedora:         sudo dnf install mingw64-gcc
#   macOS:          brew install mingw-w64
#
# Output: authlogin.dll in the current directory.

set -euo pipefail

CC="${CC:-i686-w64-mingw32-gcc}"
CFLAGS="${CFLAGS:--O2 -Wall}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "error: $CC not found in PATH" >&2
  echo "       install mingw-w64 (see header comment) or set CC=<your-gcc>" >&2
  exit 1
fi

echo "Building authlogin.dll with $CC..."
"$CC" -shared -o authlogin.dll authlogin.c -static -Wl,--subsystem,windows $CFLAGS
echo "Build successful:"
ls -l authlogin.dll
