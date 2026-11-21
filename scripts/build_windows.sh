#!/usr/bin/env bash
# Windows build, cross-compiled ON LINUX (no Windows or Visual Studio needed):
#   ./scripts/build_windows.sh /path/to/Lamborghini/default.xbe
# Downloads into ../win-deps (override with WIN_DEPS): llvm-mingw (clang for
# Windows), SDL2's MinGW package and libepoxy's MSYS2 package. Recompiles
# the XBE unless src/recomp/gen already exists (the generated code is the
# same for both platforms). Output: build-win/Lamborghini.exe plus the two
# DLLs it needs, collected in build-win/dist/.
# Needs: cmake, ninja, curl, tar with zstd, python3 + capstone.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TK="${XBOXRECOMP_DIR:-$REPO/../xboxrecomp}"
DEPS="${WIN_DEPS:-$REPO/../win-deps}"
export XBOXRECOMP_DIR="$TK"

LLVM_MINGW=llvm-mingw-20261020-msvcrt-ubuntu-22.04-x86_64
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/$LLVM_MINGW.tar.xz
SDL2_VER=2.32.10
SDL2_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-devel-$SDL2_VER-mingw.tar.gz
EPOXY_PKG=mingw-w64-x86_64-libepoxy-1.5.10-7-any.pkg.tar.zst
EPOXY_URL=https://repo.msys2.org/mingw/mingw64/$EPOXY_PKG

mkdir -p "$DEPS/dl"
fetch() { [ -f "$DEPS/dl/$2" ] || curl -fL --retry 3 -o "$DEPS/dl/$2" "$1"; }
if [ ! -d "$DEPS/$LLVM_MINGW" ]; then
    echo "== llvm-mingw"; fetch "$LLVM_MINGW_URL" "$LLVM_MINGW.tar.xz"
    tar -xJf "$DEPS/dl/$LLVM_MINGW.tar.xz" -C "$DEPS"
fi
if [ ! -d "$DEPS/SDL2" ]; then
    echo "== SDL2 $SDL2_VER"; fetch "$SDL2_URL" "SDL2-devel-$SDL2_VER-mingw.tar.gz"
    tar -xzf "$DEPS/dl/SDL2-devel-$SDL2_VER-mingw.tar.gz" -C "$DEPS" && mv "$DEPS/SDL2-$SDL2_VER" "$DEPS/SDL2"
fi
if [ ! -d "$DEPS/epoxy" ]; then
    echo "== libepoxy"; fetch "$EPOXY_URL" "$EPOXY_PKG"
    mkdir -p "$DEPS/epoxy" && tar --zstd -xf "$DEPS/dl/$EPOXY_PKG" -C "$DEPS/epoxy"
fi

if [ ! -d "$REPO/src/recomp/gen" ]; then
    "$REPO/scripts/recompile.sh" "${1:-}"
fi

echo "== building (Windows, cross-compiled)"
cd "$REPO"
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake \
    -DLLVM_MINGW_DIR="$DEPS/$LLVM_MINGW" -DXBOXRECOMP_DIR="$TK" \
    -DXBOXRECOMP_SDL_HOST=ON \
    -DSDL2_DIR="$DEPS/SDL2/x86_64-w64-mingw32/lib/cmake/SDL2" \
    -DEPOXY_DIR="$DEPS/epoxy/mingw64"
ninja -C build-win Lamborghini.exe
mkdir -p build-win/dist
cp build-win/Lamborghini.exe "$DEPS/SDL2/x86_64-w64-mingw32/bin/SDL2.dll" \
   "$DEPS/epoxy/mingw64/bin/libepoxy-0.dll" build-win/dist/

echo
echo "Done: $REPO/build-win/dist/ (Lamborghini.exe, SDL2.dll, libepoxy-0.dll)"
echo "Put them next to your Lamborghini\\ data folder."
