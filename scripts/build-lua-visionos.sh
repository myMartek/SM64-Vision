#!/usr/bin/env bash
#
# Builds Lua 5.3.5 as a static library for visionOS.
#
# sm64coopdx ships prebuilt liblua53.a for desktop platforms only (see
# sm64coopdx/lib/lua/), so the visionOS build needs its own. Lua is plain ANSI C
# with no platform assumptions, so this is a straight cross compile.
#
# coopdx's own headers from lib/lua/include are used rather than the upstream
# ones: its luaconf.h differs in the dynamic C module search paths, and the
# library has to agree with the headers the game compiles against.
#
# Output:
#   sm64coopdx/lib/lua/xros/liblua53.a
#   sm64coopdx/lib/lua/xrsimulator/liblua53.a
#
# Usage: scripts/build-lua-visionos.sh [visionos-min-version]

set -euo pipefail

LUA_VERSION="5.3.5"
LUA_SHA256="0c2eed3f960446e1a3e4b9a1ca2f3ff893b6ce41942cf54d5dd59ab4b3b058ac"
VISIONOS_MIN="${1:-26.0}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOPDX="$REPO_ROOT/sm64coopdx"
WORK="$REPO_ROOT/.build/lua"

mkdir -p "$WORK"
cd "$WORK"

TARBALL="lua-$LUA_VERSION.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "==> Downloading Lua $LUA_VERSION"
    curl -sSL -o "$TARBALL" "https://www.lua.org/ftp/$TARBALL"
fi

echo "==> Verifying checksum"
echo "$LUA_SHA256  $TARBALL" | shasum -a 256 -c -

rm -rf "lua-$LUA_VERSION"
tar xzf "$TARBALL"
SRC="$WORK/lua-$LUA_VERSION/src"

echo "==> Using coopdx's Lua headers"
cp "$COOPDX"/lib/lua/include/{lua.h,luaconf.h,lauxlib.h,lualib.h} "$SRC/"

# Excluded, and why:
#   lua.c, luac.c   have a main()
#   loslib.c        calls system(), which the visionOS SDK marks unavailable
#   liolib.c        file I/O library
#   loadlib.c       dlopen-based package loader
#   linit.c         luaL_openlibs, which pulls in all three of the above
#
# coopdx opens only base, coroutine, math, string, table and utf8 (see
# smlua.c), so none of these are referenced. Building with LUA_UNSAFE=1, which
# does expose the io/os libraries, is therefore not supported on visionOS - it
# would fail at link time with undefined luaopen_os / luaopen_io.
SOURCES=$(ls "$SRC"/*.c | grep -vE '/(lua|luac|loslib|liolib|loadlib|linit)\.c$')

build_slice() {
    local sdk="$1" triple="$2"
    local out="$COOPDX/lib/lua/$sdk"
    local obj="$WORK/obj-$sdk"

    echo "==> Building for $sdk ($triple)"
    rm -rf "$obj"; mkdir -p "$obj" "$out"

    local sysroot
    sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"

    for c in $SOURCES; do
        xcrun --sdk "$sdk" clang \
            -target "$triple" -isysroot "$sysroot" \
            -O2 -fno-common -std=gnu99 \
            -DLUA_USE_MACOSX \
            -c "$c" -o "$obj/$(basename "${c%.c}").o"
    done

    rm -f "$out/liblua53.a"
    xcrun --sdk "$sdk" ar rcs "$out/liblua53.a" "$obj"/*.o
    echo "    -> $out/liblua53.a"
}

# LUA_USE_MACOSX enables the POSIX bits (dlopen-based package loading, readline
# is not pulled in by the library itself). visionOS provides the same POSIX
# surface, and coopdx never opens the io/os/package libraries anyway, so those
# objects stay out of the final link.
build_slice xros        "arm64-apple-xros${VISIONOS_MIN}"
build_slice xrsimulator "arm64-apple-xros${VISIONOS_MIN}-simulator"

echo "==> Done"
