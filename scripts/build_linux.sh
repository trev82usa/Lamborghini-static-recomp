#!/usr/bin/env bash
# One-step Linux build: toolkit checkout + patch, recompile your default.xbe, build.
#   ./scripts/build_linux.sh /path/to/Lamborghini/default.xbe
# XBOXRECOMP_DIR overrides where the toolkit is cloned (default: ../xboxrecomp).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TK="${XBOXRECOMP_DIR:-$REPO/../xboxrecomp}"
export XBOXRECOMP_DIR="$TK"

"$REPO/scripts/recompile.sh" "${1:-}"

echo "== building (Linux)"
cd "$REPO"
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_DIR="$TK"
ninja -C build-linux Lamborghini

echo
echo "Done: $REPO/build-linux/Lamborghini.x86_64"
echo "Put it next to your Lamborghini/ data folder and run it."
