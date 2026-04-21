#!/usr/bin/env bash
# ── MakaOS pixman port (cross-gcc + sysroot) ──────────────────────────
#
# pixman is wlroots's software renderer.  Single-arch (x86_64) build,
# all SIMD backends (MMX, SSE2, SSSE3) gated by GCC __builtin_cpu_*.
# We compile the C sources + the SIMD variants the cross-gcc supports.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIXMAN_VERSION="0.42.2"
PIXMAN_URL="https://www.cairographics.org/releases/pixman-${PIXMAN_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
PIXMAN_SRC="$THIRD_PARTY/pixman-${PIXMAN_VERSION}"
PIXMAN_TARBALL="$THIRD_PARTY/pixman-${PIXMAN_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-pixman] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CONFIG_HEADER="$REPO_ROOT/scripts/configs/pixman_config.h"

CFLAGS=(
    -O2 -fPIC -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -DHAVE_CONFIG_H
)

log() { printf '[port-pixman] %s\n' "$*" >&2; }

fetch_pixman() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$PIXMAN_TARBALL" ]; then
        log "downloading pixman ${PIXMAN_VERSION}"
        curl -fSL --retry 3 -o "$PIXMAN_TARBALL" "$PIXMAN_URL"
    fi
    if [ ! -d "$PIXMAN_SRC" ]; then
        log "extracting pixman"
        tar -xzf "$PIXMAN_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/pixman-1"
    mkdir -p "$SYSROOT/usr/include/pixman-1"
    cp "$PIXMAN_SRC/pixman/pixman.h"          "$SYSROOT/usr/include/pixman-1/"
    cp "$PIXMAN_SRC/pixman/pixman-version.h.in" "$SYSROOT/usr/include/pixman-1/pixman-version.h"
    # pixman-version.h.in has @PIXMAN_VERSION_*@ tokens.  Substitute.
    sed -i \
      -e "s|@PIXMAN_VERSION_MAJOR@|0|g" \
      -e "s|@PIXMAN_VERSION_MINOR@|42|g" \
      -e "s|@PIXMAN_VERSION_MICRO@|2|g" \
      "$SYSROOT/usr/include/pixman-1/pixman-version.h"
}

build_lib() {
    local build_objs="$BUILD_DIR/pixman_objs"
    mkdir -p "$build_objs"

    cp "$CONFIG_HEADER" "$PIXMAN_SRC/pixman/config.h"

    # Identity-stub TU for the per-arch SIMD probes pixman-implementation.c
    # calls unconditionally.  Without USE_*_SIMD these must pass the
    # implementation pointer straight through.  config.h must be
    # included before pixman-private.h (enforced by an #error guard).
    cat > "$PIXMAN_SRC/pixman/pixman-arch-stubs.c" <<'STUBS'
#include "config.h"
#include "pixman-private.h"
pixman_implementation_t* _pixman_x86_get_implementations(pixman_implementation_t* imp)  { return imp; }
pixman_implementation_t* _pixman_arm_get_implementations(pixman_implementation_t* imp)  { return imp; }
pixman_implementation_t* _pixman_ppc_get_implementations(pixman_implementation_t* imp)  { return imp; }
pixman_implementation_t* _pixman_mips_get_implementations(pixman_implementation_t* imp) { return imp; }
STUBS

    local includes=(
        -I "$PIXMAN_SRC/pixman"
    )

    # Core C sources — generic implementation only.  Skip the SIMD
    # backends (mmx, sse2, ssse3) to start; they require runtime CPU
    # feature dispatch the autoconf normally generates.  Add them later
    # if pixman performance becomes a measurable wlroots bottleneck.
    local srcs=(
        "pixman/pixman.c"
        "pixman/pixman-access.c"
        "pixman/pixman-access-accessors.c"
        "pixman/pixman-bits-image.c"
        "pixman/pixman-combine32.c"
        "pixman/pixman-combine-float.c"
        "pixman/pixman-conical-gradient.c"
        "pixman/pixman-edge.c"
        "pixman/pixman-edge-accessors.c"
        "pixman/pixman-fast-path.c"
        "pixman/pixman-filter.c"
        "pixman/pixman-general.c"
        "pixman/pixman-glyph.c"
        "pixman/pixman-gradient-walker.c"
        "pixman/pixman-image.c"
        "pixman/pixman-implementation.c"
        "pixman/pixman-linear-gradient.c"
        "pixman/pixman-matrix.c"
        "pixman/pixman-noop.c"
        "pixman/pixman-radial-gradient.c"
        "pixman/pixman-region16.c"
        "pixman/pixman-region32.c"
        "pixman/pixman-solid-fill.c"
        "pixman/pixman-trap.c"
        "pixman/pixman-utils.c"
        "pixman/pixman-arch-stubs.c"   # identity stubs for SIMD probes
    )

    log "compiling ${#srcs[@]} pixman sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$PIXMAN_SRC/$name"
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libpixman-1.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/pixman_objs" "$SYSROOT/usr/lib/libpixman-1.a"
    fi
    if [ ! -f "$CONFIG_HEADER" ]; then
        log "ERROR: $CONFIG_HEADER not found"
        exit 1
    fi
    fetch_pixman
    install_headers
    build_lib
}

main "$@"
