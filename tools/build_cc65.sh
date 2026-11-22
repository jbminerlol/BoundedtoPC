#!/bin/sh
# Build ca65 + ld65 (cc65 V2.19) from source with the shared llvm-mingw toolchain.
#   tools/build_cc65.sh            -> /f/AI BS/tools/cc65/bin/{ca65,ld65}.exe
set -e
TOOLS="${EB_TOOLS:-/f/AI BS/tools}"
SRC="$TOOLS/dl/cc65-src/cc65-2.19/src"
OUT="$TOOLS/cc65/bin"
LLVM="$TOOLS/llvm-mingw-20261006-ucrt-x86_64/bin"
export PATH="$LLVM:$PATH"
export TMP="$TOOLS/tmp" TEMP="$TOOLS/tmp"
mkdir -p "$OUT" "$TOOLS/tmp"
CFLAGS="-O2 -w -DBUILD_ID=V2.19 -DCA65_INC= -DCC65_INC= -DCL65_TGT= -DLD65_LIB= -DLD65_OBJ= -DLD65_CFG="
for tool in ca65 ld65; do
  echo "building $tool"
  x86_64-w64-mingw32-clang $CFLAGS -I"$SRC/common" -o "$OUT/$tool.exe" "$SRC"/common/*.c "$SRC/$tool"/*.c
done
"$OUT/ca65.exe" --version
"$OUT/ld65.exe" --version
