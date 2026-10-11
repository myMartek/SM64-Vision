#!/usr/bin/env bash
#
# Validates the generated Metal shaders.
#
# The Metal backend compiles its shaders at runtime with newLibraryWithSource:,
# so a syntax error would only surface on the headset, at the moment the game
# first draws with that particular colour combiner. This builds the generator
# for the host, emits the shaders for every combiner SM64 uses, and pushes each
# through the offline Metal compiler instead.
#
# Usage: scripts/test-msl-shaders.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOPDX="$REPO_ROOT/sm64coopdx"
WORK="$REPO_ROOT/.build/msl-test"

rm -rf "$WORK"
mkdir -p "$WORK/shaders"

echo "==> Building the generator for the host"
clang -O1 -g -std=gnu11 \
    -I"$COOPDX/src/pc/visionos/sdl2_stub" -I"$COOPDX/src/pc/visionos" \
    -I"$COOPDX/include" -I"$COOPDX/src" -I"$COOPDX" -I"$COOPDX/lib/lua/include" \
    -DVERSION_US=1 -DF3DEX_GBI_2E=1 -DF3DEX_GBI_SHARED=1 \
    -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$WORK/msl_shader_test" \
    "$REPO_ROOT/tests/msl_shader_test.c" \
    "$COOPDX/src/pc/visionos/gfx_metal_shader.c" \
    "$COOPDX/src/pc/gfx/gfx_cc.c"

echo "==> Generating shaders"
"$WORK/msl_shader_test" "$WORK/shaders"

echo "==> Compiling each shader with the visionOS Metal compiler"
failures=0
count=0
for shader in "$WORK/shaders"/*.metal; do
    count=$((count + 1))
    if ! output=$(xcrun -sdk xros metal -std=metal3.0 -c "$shader" \
                      -o "$WORK/$(basename "${shader%.metal}").air" 2>&1); then
        echo "--- FAILED: $(basename "$shader")"
        echo "$output" | head -20
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "==> $failures of $count shaders failed to compile"
    echo "    Sources are in $WORK/shaders"
    exit 1
fi

echo "==> All $count shaders compiled"
