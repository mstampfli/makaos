#!/usr/bin/env bash
# ── MakaOS wlr-randr port ──────────────────────────────────────────────
#
# wlr-randr — display settings for wlroots compositors (sway): list
# outputs and set resolution / position / scale / transform / on-off via
# the wlr-output-management protocol.  This is the "settings" tool of the
# desktop: e.g. `wlr-randr --output ... --mode 1280x800 --scale 1`.
# swaymsg covers most live config; wlr-randr is the dedicated display
# control.  Tiny: wayland-client + one protocol, no toolkit.
#
# The wlr-output-management-unstable-v1.xml ships with our wlroots tree;
# we drop it into wlr-randr's protocol dir so meson's nofallback build
# doesn't try to fetch wlr-protocols.
#
# Produces /bin/wlr-randr.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

WR_VERSION="0.5.0"
WR_URL="https://gitlab.freedesktop.org/emersion/wlr-randr/-/archive/v${WR_VERSION}/wlr-randr-v${WR_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/wlr-randr-v${WR_VERSION}"
TARBALL="$THIRD_PARTY/wlr-randr-${WR_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/wlr_randr_build"
WLR_PROTO="$THIRD_PARTY/wlroots-0.18.1/protocol/wlr-output-management-unstable-v1.xml"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-wlr-randr] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || { log "downloading wlr-randr ${WR_VERSION}"; curl -fsSL -o "$TARBALL" "$WR_URL"; }
    [ -d "$SRC_DIR" ] || { log "extracting"; tar -xzf "$TARBALL" -C "$THIRD_PARTY"; }
    # Provide the wlr-output-management protocol locally so meson doesn't
    # need the wlr-protocols subproject.
    if [ -f "$WLR_PROTO" ]; then
        cp "$WLR_PROTO" "$SRC_DIR/wlr-output-management-unstable-v1.xml"
        # Point meson.build at the local copy if it references a subproject.
        sed -i "s#[^ ']*wlr-output-management-unstable-v1.xml#wlr-output-management-unstable-v1.xml#g" "$SRC_DIR/meson.build" 2>/dev/null || true
    fi
}

configure() {
    [ "${FORCE:-0}" = "1" ] && rm -rf "$OUT_DIR"
    [ -f "$OUT_DIR/build.ninja" ] && return 0
    log "meson setup"
    meson setup "$OUT_DIR" "$SRC_DIR" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr --libdir=lib --buildtype=release --wrap-mode=nofallback \
        -Dc_args="-Wno-error" \
        2>&1 | tee "$BUILD_DIR/wlr_randr_meson.log" | tail -6
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" wlr-randr 2>&1 | tee "$BUILD_DIR/wlr_randr_ninja.log" | tail -5
    mkdir -p "$SYSROOT/usr/bin"
    install -m755 "$OUT_DIR/wlr-randr" "$SYSROOT/usr/bin/wlr-randr"
    log "done — $(stat -c%s "$SYSROOT/usr/bin/wlr-randr") bytes"
}

main() { fetch; configure; build_install; }
main "$@"
