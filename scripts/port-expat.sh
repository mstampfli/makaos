#!/usr/bin/env bash
# ── MakaOS libexpat port (cross-gcc + sysroot) ────────────────────────
#
# Pure-C XML parser.  Single `expat` directory with 3 source files
# + 1 public header.  No autoconf dance needed — we just compile
# the library sources and install expat.h.
#
# Required by fontconfig (Tier 2) + wayland-scanner (Tier 4) + any
# XML-consuming library downstream.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPAT_VERSION="2.6.4"
# GitHub release tarball: name style R_2_6_4
EXPAT_URL="https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-${EXPAT_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
EXPAT_SRC="$THIRD_PARTY/expat-${EXPAT_VERSION}"
EXPAT_TARBALL="$THIRD_PARTY/expat-${EXPAT_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-expat] FATAL: cross-toolchain missing" >&2
    exit 1
fi

# expat needs to know which byte order it's on + which hashing to use
# for entropy.  BYTE_ORDER is x86_64 (LE).  For the random seed, we
# pick XML_POOR_ENTROPY to avoid needing getrandom/arc4random —
# our libc exposes /dev/urandom but expat's probe doesn't see it.
CONFIG_HEADER="$REPO_ROOT/scripts/configs/expat_config.h"

# -include injects our config header before every TU (matches what
# autotools does when HAVE_EXPAT_CONFIG_H is set).
CFLAGS=(
    -O2
    -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -include "$CONFIG_HEADER"
)

log() { printf '[port-expat] %s\n' "$*" >&2; }

fetch_expat() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$EXPAT_TARBALL" ]; then
        log "downloading expat ${EXPAT_VERSION}"
        curl -fSL --retry 3 -o "$EXPAT_TARBALL" "$EXPAT_URL"
    fi
    if [ ! -d "$EXPAT_SRC" ]; then
        log "extracting expat"
        tar -xJf "$EXPAT_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include"
    mkdir -p "$SYSROOT/usr/include"
    cp "$EXPAT_SRC/lib/expat.h"          "$SYSROOT/usr/include/expat.h"
    cp "$EXPAT_SRC/lib/expat_external.h" "$SYSROOT/usr/include/expat_external.h"
}

build_lib() {
    local build_objs="$BUILD_DIR/expat_objs"
    mkdir -p "$build_objs"

    local srcs=( xmlparse.c xmltok.c xmlrole.c )
    log "compiling ${#srcs[@]} expat sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$EXPAT_SRC/lib/$name"
        local obj="$build_objs/${name%.c}.o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" \
                -I "$(dirname "$CONFIG_HEADER")" \
                -I "$EXPAT_SRC/lib" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libexpat.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/expat_objs" "$SYSROOT/usr/lib/libexpat.a"
    fi
    if [ ! -d "$SYSROOT/usr/include" ]; then
        log "ERROR: sysroot not populated — run ./build.sh first"
        exit 1
    fi
    fetch_expat
    install_headers
    build_lib
}

main "$@"
