#!/bin/sh
# Build EarthBound for Windows (x86_64, clang/llvm-mingw, SDL3).
#   ./build.sh [Debug|Release]
# Steps: reassemble ebsrc from your ROM (once), build the program database, recompile the 65816
# code to C, then compile everything. Output: build/<cfg>/EarthBound.exe (+ SDL3.dll).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS="${EB_TOOLS:-/f/AI BS/tools}"
CFG="${1:-Release}"
BUILD="$HERE/build/$CFG"
LLVM="$TOOLS/llvm-mingw-20261006-ucrt-x86_64/bin"
export TMP="$HERE/build/tmp" TEMP="$HERE/build/tmp"
mkdir -p "$HERE/build/tmp"
export PATH="$LLVM:$TOOLS/ninja:$TOOLS/cmake-4.4.4-windows-x86_64/bin:$PATH"
cd "$HERE"
if [ ! -f build/ebsrc/earthbound.dbg ]; then
  python -I tools/build_ebsrc.py --jobs "${EB_JOBS:-3}"
fi
if [ ! -f build/ebsrc/program.pickle ] || [ build/ebsrc/earthbound.dbg -nt build/ebsrc/program.pickle ]; then
  python -I tools/dbginfo.py
fi
if [ ! -f build/gen/eb_gen.h ] || [ tools/recomp.py -nt build/gen/eb_gen.h ] || \
   [ build/ebsrc/program.pickle -nt build/gen/eb_gen.h ]; then
  python -I tools/recomp.py
fi
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CFG" \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DSDL3_DIR="$TOOLS/SDL3-3.4.18/x86_64-w64-mingw32/lib/cmake/SDL3" >/dev/null
fi
cmake --build "$BUILD" -j "${EB_JOBS:-3}"
cp -u "$TOOLS/SDL3-3.4.18/x86_64-w64-mingw32/bin/SDL3.dll" "$BUILD/"
echo "built $BUILD/EarthBound.exe"
# the player's copy (its own saves/ and settings); skipped while that copy is running
if [ "$CFG" = "Release" ] && [ -z "$EB_NO_PLAY_COPY" ]; then
  PLAY="$(dirname "$HERE")/EarthBound Windows"
  mkdir -p "$PLAY"
  if cp -f "$BUILD/EarthBound.exe" "$PLAY/EarthBound.exe" 2>/dev/null; then
    cp -u "$BUILD/SDL3.dll" "$PLAY/"
    echo "updated $PLAY"
  else
    echo "note: $PLAY/EarthBound.exe is in use (left unchanged)"
  fi
fi
