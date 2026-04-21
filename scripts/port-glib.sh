#!/usr/bin/env bash
# ── MakaOS GLib port ───────────────────────────────────────────────────
#
# Core GNOME foundation library.  Pango and anything downstream in the
# GNOME stack depend on it.  Big: ~800 KLOC.  Disable absolutely every
# optional feature meson exposes so we only build libglib-2.0 +
# libgobject-2.0 + libgio-2.0 + gthread + gmodule — the minimum.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLIB_VERSION="2.82.2"
GLIB_URL="https://download.gnome.org/sources/glib/2.82/glib-${GLIB_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
GLIB_SRC="$THIRD_PARTY/glib-${GLIB_VERSION}"
GLIB_TARBALL="$THIRD_PARTY/glib-${GLIB_VERSION}.tar.xz"
GLIB_BUILD="$BUILD_DIR/glib_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

log() { printf '[port-glib] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$GLIB_TARBALL" ] || curl -fsSL -o "$GLIB_TARBALL" "$GLIB_URL"
    [ -d "$GLIB_SRC" ]     || tar -xJf "$GLIB_TARBALL" -C "$THIRD_PARTY"
}

# pkg-config needs to see sysroot .pc files and prefix emitted paths.
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$GLIB_BUILD"
    fi
    [ ! -d "$GLIB_BUILD" ] || return 0
    log "meson setup"
    meson setup "$GLIB_BUILD" "$GLIB_SRC" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        -Dtests=false \
        -Dxattr=false \
        -Dselinux=disabled \
        -Dlibmount=disabled \
        -Dsysprof=disabled \
        -Ddocumentation=false \
        -Dman-pages=disabled \
        -Dintrospection=disabled \
        -Dnls=disabled \
        -Dinstalled_tests=false \
        -Ddtrace=disabled \
        -Dsystemtap=disabled \
        -Dglib_debug=disabled \
        -Dglib_assert=false \
        -Dglib_checks=false \
        -Doss_fuzz=disabled \
        2>&1 | tee "$BUILD_DIR/glib_meson.log"
}

build_install() {
    log "ninja build"
    ninja -C "$GLIB_BUILD" 2>&1 | tee "$BUILD_DIR/glib_ninja.log"
    log "install"
    DESTDIR="$SYSROOT" ninja -C "$GLIB_BUILD" install 2>&1 | tee "$BUILD_DIR/glib_install.log"
}

main() {
    fetch
    configure
    build_install
    log "done"
    ls -la "$SYSROOT"/usr/lib/libglib* "$SYSROOT"/usr/lib/libgobject* "$SYSROOT"/usr/lib/libgio* 2>/dev/null | awk '{print $5, $NF}'
}
main "$@"
