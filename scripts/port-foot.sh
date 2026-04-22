#!/usr/bin/env bash
# ── MakaOS foot port ──────────────────────────────────────────────────
#
# Foot is a fast, small (~25 KLOC), independent Wayland terminal
# emulator written in C.  This is what you'll spawn from dwl to get
# a bash prompt in a window.
#
# Deps: wayland-client, xkbcommon, pixman, freetype, fontconfig,
# harfbuzz, fcft, tllist (header-only).  harfbuzz is required for
# complex-script shaping — upstream treats it as mandatory.
#
# Produces /bin/foot once the build.sh install step copies it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

FOOT_VERSION="1.20.2"
FOOT_URL="https://codeberg.org/dnkl/foot/archive/${FOOT_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/foot"
TARBALL="$THIRD_PARTY/foot-${FOOT_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/foot_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-foot] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading foot ${FOOT_VERSION}"
        curl -fsSL -o "$TARBALL" "$FOOT_URL"
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
        -Dterminfo=disabled \
        -Dgrapheme-clustering=enabled \
        -Dime=false \
        -Ddocs=disabled \
        -Dthemes=false \
        -Dtests=false \
        -Dc_args="-D__STDC_ISO_10646__=201706L -Wno-error=deprecated-declarations -Wno-error=unused-variable -Wno-error=return-type -Wno-error=format" \
        2>&1 | tee "$BUILD_DIR/foot_meson.log"
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" foot 2>&1 | tee "$BUILD_DIR/foot_ninja.log"

    mkdir -p "$SYSROOT/usr/bin"
    install -m755 "$OUT_DIR/foot" "$SYSROOT/usr/bin/foot"
    log "done — $(stat -c%s "$SYSROOT/usr/bin/foot") bytes → $SYSROOT/usr/bin/foot"
}

main() { fetch; configure; build_install; }
main "$@"
