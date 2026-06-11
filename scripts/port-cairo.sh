#!/usr/bin/env bash
# ── MakaOS cairo port ──────────────────────────────────────────────────
#
# 2D vector graphics over pixman.  sway's titlebar/text stack needs
# cairo + cairo-ft (freetype font backend); the PNG surface rides along
# for swaybar's image fallback.  Image+ft only: no xlib/xcb/gl.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAIRO_VERSION="1.18.2"
CAIRO_URL="https://cairographics.org/releases/cairo-${CAIRO_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
CAIRO_SRC="$THIRD_PARTY/cairo-${CAIRO_VERSION}"
CAIRO_TARBALL="$THIRD_PARTY/cairo-${CAIRO_VERSION}.tar.xz"
CAIRO_BUILD="$BUILD_DIR/cairo_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

log() { printf '[port-cairo] %s\n' "$*" >&2; }

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$CAIRO_TARBALL" ] || {
        log "downloading cairo ${CAIRO_VERSION}"
        curl -fSL --retry 3 -o "$CAIRO_TARBALL" "$CAIRO_URL"
    }
    [ -d "$CAIRO_SRC" ] || {
        log "extracting cairo"
        tar -xJf "$CAIRO_TARBALL" -C "$THIRD_PARTY"
    }
}

build() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$CAIRO_BUILD"
    fi
    if [ ! -d "$CAIRO_BUILD" ]; then
        log "meson setup"
        meson setup "$CAIRO_BUILD" "$CAIRO_SRC" \
            --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
            --prefix=/usr \
            --libdir=lib \
            --wrap-mode=nofallback \
            -Ddefault_library=static \
            -Dfontconfig=enabled \
            -Dfreetype=enabled \
            -Dpng=enabled \
            -Dzlib=enabled \
            -Dxlib=disabled \
            -Dxcb=disabled \
            -Dquartz=disabled \
            -Ddwrite=disabled \
            -Dtests=disabled \
            -Dgtk_doc=false \
            -Dspectre=disabled \
            -Dsymbol-lookup=disabled \
            2>&1 | tee "$BUILD_DIR/cairo_meson.log" | tail -3
    fi
    log "ninja build"
    ninja -C "$CAIRO_BUILD" 2>&1 | tail -3
    log "installing into $SYSROOT"
    DESTDIR="$SYSROOT" ninja -C "$CAIRO_BUILD" install > /dev/null
    log "done — $(ls "$SYSROOT"/usr/lib/libcairo.a)"
}

main() {
    fetch
    build
}
main "$@"
