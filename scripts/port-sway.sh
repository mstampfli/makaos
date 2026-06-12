#!/usr/bin/env bash
# ── MakaOS sway port ───────────────────────────────────────────────────
#
# The i3-compatible Wayland compositor — the userspace goal.  Built
# against the ported wlroots 0.18.1 stack with the minimal feature set:
# no tray (needs sd-bus), no swaybar/swaynag binaries yet (they link
# fine but double the surface — enable once the core is proven), no
# gdk-pixbuf, no man pages.  Installs sway + swaymsg + the default
# config into the sysroot.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SWAY_VERSION="1.10.1"
SWAY_URL="https://github.com/swaywm/sway/archive/refs/tags/${SWAY_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SWAY_SRC="$THIRD_PARTY/sway-${SWAY_VERSION}"
SWAY_TARBALL="$THIRD_PARTY/sway-${SWAY_VERSION}.tar.gz"
SWAY_BUILD="$BUILD_DIR/sway_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH

log() { printf '[port-sway] %s\n' "$*" >&2; }

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$SWAY_TARBALL" ] || {
        log "downloading sway ${SWAY_VERSION}"
        curl -fSL --retry 3 -o "$SWAY_TARBALL" "$SWAY_URL"
    }
    [ -d "$SWAY_SRC" ] || {
        log "extracting sway"
        tar -xzf "$SWAY_TARBALL" -C "$THIRD_PARTY"
    }
}

build() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$SWAY_BUILD"
    fi
    if [ ! -d "$SWAY_BUILD" ]; then
        log "meson setup"
        meson setup "$SWAY_BUILD" "$SWAY_SRC" \
            --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
            --prefix=/usr \
            --libdir=lib \
            --wrap-mode=nofallback \
            -Ddefault_library=static \
            -Dswaybar=true \
            -Dswaynag=true \
            -Dtray=disabled \
            -Dgdk-pixbuf=disabled \
            -Dman-pages=disabled \
            -Dbash-completions=false \
            -Dzsh-completions=false \
            -Dfish-completions=false \
            -Ddefault-wallpaper=false \
            -Dwerror=false \
            2>&1 | tee "$BUILD_DIR/sway_meson.log" | tail -3
    fi
    log "ninja build"
    ninja -C "$SWAY_BUILD" 2>&1 | tail -3
    log "installing into $SYSROOT"
    DESTDIR="$SYSROOT" ninja -C "$SWAY_BUILD" install > /dev/null
    log "done — $(ls "$SYSROOT"/usr/bin/sway)"
}

main() {
    fetch
    build
}
main "$@"
