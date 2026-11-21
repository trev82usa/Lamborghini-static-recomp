#!/usr/bin/env bash
# One-step Linux build: toolkit checkout + patch, recompile your default.xbe, build.
#   ./scripts/build_linux.sh /path/to/Lamborghini/default.xbe
# XBOXRECOMP_DIR overrides where the toolkit is cloned (default: ../xboxrecomp).
set -euo pipefail

XBE="${1:-}"
if [ -z "$XBE" ] || [ ! -f "$XBE" ]; then
    echo "usage: $0 /path/to/Lamborghini/default.xbe" >&2
    exit 1
fi
XBE="$(cd "$(dirname "$XBE")" && pwd)/$(basename "$XBE")"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TK="${XBOXRECOMP_DIR:-$REPO/../xboxrecomp}"
BASE="$(tr -d '[:space:]' < "$REPO/patches/xboxrecomp-base-commit.txt")"
GEN="$REPO/src/recomp/gen"

python3 -c "import capstone" 2>/dev/null || {
    echo "Python module 'capstone' is missing (apt: python3-capstone, pip: capstone)" >&2
    exit 1
}

echo "== toolkit: $TK @ ${BASE:0:10}"
if [ ! -d "$TK/.git" ]; then
    git clone https://github.com/sp00nznet/xboxrecomp "$TK"
fi
cd "$TK"
if git apply --reverse --check "$REPO/patches/xboxrecomp-lamborghini.patch" 2>/dev/null; then
    echo "   patch already applied"
else
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        echo "$TK has local changes; use a clean clone (or set XBOXRECOMP_DIR)" >&2
        exit 1
    fi
    git fetch -q origin || true
    git checkout -q "$BASE"
    git apply "$REPO/patches/xboxrecomp-lamborghini.patch"
fi

echo "== recompiling $XBE (several minutes)"
mkdir -p game
cp "$XBE" game/default.xbe
python3 -m tools.xbe_parser game/default.xbe --json game/default_analysis.json
python3 -m tools.disasm game/default.xbe -v --seed-functions "$REPO/config/seed_functions.json"
python3 -m tools.func_id game/default.xbe -v
python3 -m tools.abi_analysis game/default.xbe -v
rm -rf "${GEN:?}"
python3 -m tools.recomp game/default.xbe --all --split 250 --gen-dir "$GEN"
python3 "$REPO/scripts/post_recomp_bink_fix.py" "$GEN"

echo "== building"
cd "$REPO"
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_DIR="$TK"
ninja -C build-linux Lamborghini

echo
echo "Done: $REPO/build-linux/Lamborghini.x86_64"
echo "Put it next to your Lamborghini/ data folder and run it."
