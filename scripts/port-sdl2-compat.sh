#!/usr/bin/env bash
# Build sdl2-compat (the SDL2 API implemented over SDL3) as libSDL2.a for MakaOS,
# UNMODIFIED.  Pinned to release-2.30.50 -- the sdl2-compat that targets SDL3
# 3.2.0 (exactly our SDL3); newer sdl2-compat needs SDL 3.2.4's integer_x/y wheel
# API, which our 3.2.0 headers lack.  The shim loads SDL3 the normal way -- raw
# dlopen("libSDL3.so", RTLD_NOW) + a dlsym'd SDL3_##fn jump table -- which the
# MakaOS in-libc dlopen loader serves (bare soname resolves to /lib/libSDL3.so).
# So DarkPlaces/Xonotic link -lSDL2 and get SDL3 at runtime, no static-bind hack.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd)"
SYSROOT="${SYSROOT:-$REPO/build/sysroot}"
SC="$REPO/build/third_party/sdl2-compat"
SRC="$SC/src"
CC="$REPO/toolchain/bin/x86_64-pc-makaos-gcc"
AR="$REPO/toolchain/bin/x86_64-pc-makaos-ar"
TAG=release-2.30.50
B="$REPO/build/sdl2compat_build"

[ -f "$SYSROOT/usr/include/SDL3/SDL.h" ] || { echo "FATAL: SDL3 headers missing (run port-sdl3 first)"; exit 1; }
[ -d "$SC/.git" ] || { echo "FATAL: sdl2-compat git checkout missing at $SC"; exit 1; }

# Pin the SDL3-3.2.0-compatible tag (fetch if this shallow clone lacks it).
if [ "$(git -C "$SC" describe --tags 2>/dev/null)" != "$TAG" ]; then
    git -C "$SC" fetch --depth 1 --quiet origin tag "$TAG"
    git -C "$SC" checkout -q "$TAG"
fi

# INCLUDES points at the SDL3 headers (per the shim's own Makefile note); the
# shim's SDL2-side types come from its internal headers via relative includes.
CFLAGS="--sysroot=$SYSROOT -fPIC -fvisibility=hidden -O2 -Wall \
  -DNDEBUG -D_THREAD_SAFE -D_REENTRANT -DSDL_INCLUDE_STDBOOL_H \
  -isystem $SYSROOT/usr/include"

mkdir -p "$B/dynapi"
"$CC" $CFLAGS -c "$SRC/sdl2_compat.c"        -o "$B/sdl2_compat.o"
"$CC" $CFLAGS -c "$SRC/dynapi/SDL_dynapi.c"  -o "$B/SDL_dynapi.o"
"$AR" rcs "$SYSROOT/usr/lib/libSDL2.a" "$B/sdl2_compat.o" "$B/SDL_dynapi.o"

# Install the public SDL2 headers so games (DarkPlaces) can #include <SDL2/SDL.h>.
mkdir -p "$SYSROOT/usr/include/SDL2"
cp "$SC/include/SDL2/"*.h "$SYSROOT/usr/include/SDL2/"

echo "[port-sdl2-compat] libSDL2.a ($(stat -c%s "$SYSROOT/usr/lib/libSDL2.a") bytes) + $(ls "$SYSROOT/usr/include/SDL2/"*.h | wc -l) SDL2 headers installed ($TAG)"
