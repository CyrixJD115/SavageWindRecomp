#!/bin/sh
# Builds the hactool the setup reads the player's own NSP / XCI game packages with (runtime/game_import.h): SciresM/hactool
# (ISC) at the commit totk-pc-recomp builds (its tools/build_hactool.py is the recipe and tools/hactool/*.patch carry
# the same three patches), into <out-dir>/hactool[.exe] + LICENSE. `make` runs it (build/hactool/, next to the
# executable, where runtime/paths.c rt_resource looks); the releases ship that folder. Keys are never part of this:
# the binary is keyless; the player's keys are read at run time.
#
#   tools/build_hactool.sh [src-dir] [out-dir]
#
# Needs git, a C compiler (CC, default cc) and make; hactool's bundled mbedtls builds with it.
set -e
COMMIT=1d64a83450e025622f3468c28fc4164dad2c5ef6
SRC=${1:-build/hactool-src}
OUT=${2:-build/hactool}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac

mkdir -p "$SRC"
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/SciresM/hactool.git
fi
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$COMMIT" ]; then
    git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
fi
git -C "$SRC" checkout -q -f "$COMMIT"
git -C "$SRC" clean -qfdx
for patch in "$HERE"/hactool/portable.patch "$HERE"/hactool/unicode.patch "$HERE"/hactool/mbedtls-asm-clobbers.patch; do
    git -C "$SRC" apply --whitespace=nowarn "$patch"
done
printf 'CC = %s\nEXEEXT = %s\nCFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11\nLDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto\n' "$CC" "$EXE" > "$SRC/config.mk"
make -C "$SRC" -j$(nproc 2>/dev/null || echo 4) CC="$CC"

mkdir -p "$OUT"
cp "$SRC/hactool$EXE" "$OUT/hactool$EXE"
cp "$SRC/LICENSE" "$OUT/LICENSE"
# hactool reads ~/.switch/prod.keys on start; with an empty home this checks the binary, not the player's keys.
TMP=$(mktemp -d)
if HOME="$TMP" USERPROFILE="$TMP" "$OUT/hactool$EXE" 2>&1 | grep -q 'hactool (c) SciresM'; then
    echo "hactool built: $OUT/hactool$EXE ($COMMIT)"
else
    echo "the hactool binary doesn't start" >&2
    exit 1
fi
rm -rf "$TMP"
