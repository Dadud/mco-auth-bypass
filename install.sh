#!/usr/bin/env bash
# Drop authlogin.dll into another project's install tree.
#
# Usage:
#   ./install.sh <path-to-target-dir>
#
# The target dir is expected to be the game's "update" directory
# (the one containing MCity_d.exe / MCity.exe). authlogin.dll is copied
# there. The DLL must be built first (run ./build.sh).

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <target-install-dir>" >&2
  exit 1
fi

target="$1"

if [[ ! -d "$target" ]]; then
  echo "error: $target is not a directory" >&2
  exit 1
fi

if [[ ! -f authlogin.dll ]]; then
  echo "error: authlogin.dll not found in current directory; run ./build.sh first" >&2
  exit 1
fi

cp -v authlogin.dll "$target/authlogin.dll"
echo "Installed authlogin.dll -> $target"
