#!/usr/bin/env bash
# ── MakaOS libdisplay-info port ────────────────────────────────────────
#
# EDID / DisplayID parser.  Required by wlroots' DRM backend.  Pure C,
# ~12 KLOC, meson build, no external deps beyond hwdata's pnp.ids.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DI_VERSION="0.2.0"
DI_URL="https://gitlab.freedesktop.org/emersion/libdisplay-info/-/archive/${DI_VERSION}/libdisplay-info-${DI_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
DI_SRC="$THIRD_PARTY/libdisplay-info-${DI_VERSION}"
DI_TARBALL="$THIRD_PARTY/libdisplay-info-${DI_VERSION}.tar.gz"
DI_BUILD="$BUILD_DIR/libdisplay_info_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-di] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$DI_TARBALL" ] || curl -fsSL -o "$DI_TARBALL" "$DI_URL"
    [ -d "$DI_SRC" ]     || tar -xzf "$DI_TARBALL" -C "$THIRD_PARTY"
}

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 wiping build dir"
        rm -rf "$DI_BUILD"
    fi
    [ -d "$DI_BUILD" ] && return 0
    log "meson setup"
    meson setup "$DI_BUILD" "$DI_SRC" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        2>&1 | tee "$BUILD_DIR/libdisplay_info_meson.log"
}

build_install() {
    log "ninja build"
    ninja -C "$DI_BUILD" 2>&1 | tee "$BUILD_DIR/libdisplay_info_ninja.log"
    DESTDIR="$SYSROOT" ninja -C "$DI_BUILD" install \
        2>&1 | tee "$BUILD_DIR/libdisplay_info_install.log"
}

main() {
    # Ensure hwdata is in place (libdisplay-info hard-needs pnp.ids).
    "$REPO_ROOT/scripts/port-hwdata.sh"
    fetch
    configure
    build_install
    log "done"
    ls -la "$SYSROOT"/usr/lib/libdisplay-info* 2>/dev/null
}
main "$@"
