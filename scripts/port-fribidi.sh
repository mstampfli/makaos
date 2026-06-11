#!/usr/bin/env bash
# ── MakaOS fribidi port ────────────────────────────────────────────────
#
# Unicode bidirectional-text algorithm library.  Pure computation, no
# OS surface beyond malloc/stdio — ports clean.  Needed transitively by
# pango (sway's titlebar text stack: pango → fribidi).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FRIBIDI_VERSION="1.0.16"
FRIBIDI_URL="https://github.com/fribidi/fribidi/releases/download/v${FRIBIDI_VERSION}/fribidi-${FRIBIDI_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
FRIBIDI_SRC="$THIRD_PARTY/fribidi-${FRIBIDI_VERSION}"
FRIBIDI_TARBALL="$THIRD_PARTY/fribidi-${FRIBIDI_VERSION}.tar.xz"
FRIBIDI_BUILD="$BUILD_DIR/fribidi_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

log() { printf '[port-fribidi] %s\n' "$*" >&2; }

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$FRIBIDI_TARBALL" ] || {
        log "downloading fribidi ${FRIBIDI_VERSION}"
        curl -fSL --retry 3 -o "$FRIBIDI_TARBALL" "$FRIBIDI_URL"
    }
    [ -d "$FRIBIDI_SRC" ] || {
        log "extracting fribidi"
        tar -xJf "$FRIBIDI_TARBALL" -C "$THIRD_PARTY"
    }
    # fribidi force-adds -ansi (C90) when gcc accepts it, which rejects
    # the // comments in MakaOS sysroot headers.  Drop it — the library
    # compiles fine as gnu11.
    if grep -q "add_project_arguments('-ansi'" "$FRIBIDI_SRC/meson.build"; then
        log "patching out -ansi"
        sed -i "s/add_project_arguments('-ansi', language: 'c')/# MakaOS: -ansi removed — sysroot headers use \/\/ comments/" \
            "$FRIBIDI_SRC/meson.build"
    fi
}

build() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$FRIBIDI_BUILD"
    fi
    if [ ! -d "$FRIBIDI_BUILD" ]; then
        log "meson setup"
        meson setup "$FRIBIDI_BUILD" "$FRIBIDI_SRC" \
            --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
            --prefix=/usr \
            --libdir=lib \
            -Ddefault_library=static \
            -Ddocs=false \
            -Dtests=false \
            -Dbin=false \
            -Dc_std=gnu11 \
            2>&1 | tee "$BUILD_DIR/fribidi_meson.log"
    fi
    log "ninja build"
    ninja -C "$FRIBIDI_BUILD" 2>&1 | tail -3
    log "installing into $SYSROOT"
    DESTDIR="$SYSROOT" ninja -C "$FRIBIDI_BUILD" install > /dev/null
    log "done — $(ls "$SYSROOT"/usr/lib/libfribidi.a)"
}

main() {
    fetch
    build
}
main "$@"
