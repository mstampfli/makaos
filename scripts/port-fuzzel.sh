#!/usr/bin/env bash
# ── MakaOS fuzzel port ─────────────────────────────────────────────────
#
# fuzzel — a wlroots-native application launcher / dmenu replacement.
# Shares foot's entire dependency set (fcft + pixman + fontconfig +
# wayland-client + wlr-layer-shell), so it's the cleanest launcher to
# bring up: no GTK, no extra toolkit.  Renders directly with pixman.
#
# Deps: wayland-client, wayland-protocols (wlr-layer-shell), xkbcommon,
# pixman, fcft, fontconfig, tllist (header-only), libpng (icon PNGs).
#
# Produces /bin/fuzzel.  Run as the launcher; in the sway config:
#   set $menu fuzzel
#   bindsym $mod+d exec $menu

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

FUZZEL_VERSION="1.11.1"
FUZZEL_URL="https://codeberg.org/dnkl/fuzzel/archive/${FUZZEL_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/fuzzel"
TARBALL="$THIRD_PARTY/fuzzel-${FUZZEL_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/fuzzel_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-fuzzel] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading fuzzel ${FUZZEL_VERSION}"
        curl -fsSL -o "$TARBALL" "$FUZZEL_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        # codeberg archives unpack to fuzzel/ already
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
        [ -d "$SRC_DIR" ] || mv "$THIRD_PARTY"/fuzzel-* "$SRC_DIR" 2>/dev/null || true
    }
}

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping"
        rm -rf "$OUT_DIR"
    fi
    [ -f "$OUT_DIR/build.ninja" ] && return 0
    # No scdoc on the host toolchain → drop the man-page subdir (fuzzel
    # has no meson option to disable docs).  Idempotent.
    sed -i "s/^subdir('doc')/# subdir('doc') — disabled: no scdoc/" \
        "$SRC_DIR/meson.build"
    log "meson setup"
    meson setup "$OUT_DIR" "$SRC_DIR" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        --buildtype=release \
        --wrap-mode=nofallback \
        -Denable-cairo=disabled \
        -Dpng-backend=none \
        -Dsvg-backend=none \
        -Dc_args="-D__STDC_ISO_10646__=201706L -Wno-error" \
        2>&1 | tee "$BUILD_DIR/fuzzel_meson.log" | tail -5
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" fuzzel 2>&1 | tee "$BUILD_DIR/fuzzel_ninja.log" | tail -5
    mkdir -p "$SYSROOT/usr/bin"
    install -m755 "$OUT_DIR/fuzzel" "$SYSROOT/usr/bin/fuzzel"
    log "done — $(stat -c%s "$SYSROOT/usr/bin/fuzzel") bytes → $SYSROOT/usr/bin/fuzzel"
}

main() { fetch; configure; build_install; }
main "$@"
