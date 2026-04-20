#!/usr/bin/env bash
# ── MakaOS zlib port (cross-gcc + sysroot) ────────────────────────────
#
# Fetches pinned zlib, compiles with x86_64-pc-makaos-gcc, installs
# libz.a + zlib.h + zconf.h into the sysroot.  No per-port shims.
#
# zlib auto-configures via configure script normally; we bypass it
# because zconf.h ships in a form that works for any POSIX-ish target.
# All configure adds is shared-library / PIC plumbing we don't want.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZLIB_VERSION="1.3.1"
ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
ZLIB_SRC="$THIRD_PARTY/zlib-${ZLIB_VERSION}"
ZLIB_TARBALL="$THIRD_PARTY/zlib-${ZLIB_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-zlib] FATAL: cross-toolchain missing — run scripts/build-toolchain.sh" >&2
    exit 1
fi

# zlib is clean C, no threading, no locale — our sysroot has all it
# needs.  -DHAVE_HIDDEN hides some non-API symbols.
CFLAGS=(
    -O2
    -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -DHAVE_HIDDEN
    -DZ_HAVE_UNISTD_H
)

log() { printf '[port-zlib] %s\n' "$*" >&2; }

fetch_zlib() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$ZLIB_TARBALL" ]; then
        log "downloading zlib ${ZLIB_VERSION}"
        curl -fSL --retry 3 -o "$ZLIB_TARBALL" "$ZLIB_URL"
    fi
    if [ ! -d "$ZLIB_SRC" ]; then
        log "extracting zlib"
        tar -xzf "$ZLIB_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include"
    mkdir -p "$SYSROOT/usr/include"
    cp "$ZLIB_SRC/zlib.h"  "$SYSROOT/usr/include/zlib.h"
    cp "$ZLIB_SRC/zconf.h" "$SYSROOT/usr/include/zconf.h"
}

# zlib source list.  We take every *.c in the top directory except the
# command-line tools (minigzip, example) — those are executables, not
# library TUs.
build_lib() {
    local build_objs="$BUILD_DIR/zlib_objs"
    mkdir -p "$build_objs"

    local srcs=(
        adler32.c crc32.c deflate.c infback.c inffast.c inflate.c
        inftrees.c trees.c zutil.c compress.c uncompr.c gzclose.c
        gzlib.c gzread.c gzwrite.c
    )

    log "compiling ${#srcs[@]} zlib sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$ZLIB_SRC/$name"
        local obj="$build_objs/${name%.c}.o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libz.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/zlib_objs" "$SYSROOT/usr/lib/libz.a"
    fi
    if [ ! -d "$SYSROOT/usr/include" ]; then
        log "ERROR: sysroot not populated — run ./build.sh first"
        exit 1
    fi

    fetch_zlib
    install_headers
    build_lib
}

main "$@"
