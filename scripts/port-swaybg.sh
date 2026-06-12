#!/usr/bin/env bash
# ── MakaOS swaybg port ─────────────────────────────────────────────────
#
# swaybg — wallpaper daemon for wlroots compositors (sway).  Draws the
# desktop background via the wlr-layer-shell.  Built WITHOUT gdk-pixbuf
# (a heavy GLib/loader stack); cairo's built-in PNG loader + solid
# colours cover the common cases.  PNG wallpapers and `solid_color`
# work; JPEG/other formats would need gdk-pixbuf.
#
# Deps: wayland-client, wayland-protocols (wlr-layer-shell, viewporter,
# single-pixel-buffer), cairo (PNG).
#
# Produces /bin/swaybg.  In the sway config:
#   output * bg /usr/share/backgrounds/sway/Wallpaper.png fill
#   output * bg #1a1b26 solid_color

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

SWAYBG_VERSION="1.2.1"
SWAYBG_URL="https://github.com/swaywm/swaybg/archive/refs/tags/v${SWAYBG_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/swaybg-${SWAYBG_VERSION}"
TARBALL="$THIRD_PARTY/swaybg-${SWAYBG_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/swaybg_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-swaybg] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading swaybg ${SWAYBG_VERSION}"
        curl -fsSL -o "$TARBALL" "$SWAYBG_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
    }
}

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping"
        rm -rf "$OUT_DIR"
    fi
    [ -f "$OUT_DIR/build.ninja" ] && return 0
    log "meson setup"
    meson setup "$OUT_DIR" "$SRC_DIR" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        --buildtype=release \
        --wrap-mode=nofallback \
        -Dgdk-pixbuf=disabled \
        -Dman-pages=disabled \
        -Dc_args="-Wno-error" \
        2>&1 | tee "$BUILD_DIR/swaybg_meson.log" | tail -5
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" swaybg 2>&1 | tee "$BUILD_DIR/swaybg_ninja.log" | tail -5
    mkdir -p "$SYSROOT/usr/bin"
    install -m755 "$OUT_DIR/swaybg" "$SYSROOT/usr/bin/swaybg"
    log "done — $(stat -c%s "$SYSROOT/usr/bin/swaybg") bytes → $SYSROOT/usr/bin/swaybg"
}

main() { fetch; configure; build_install; }
main "$@"
