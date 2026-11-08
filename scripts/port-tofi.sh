#!/usr/bin/env bash
# ── MakaOS tofi port ───────────────────────────────────────────────────
#
# tofi — a fast, single-binary dmenu/rofi-style launcher for wlroots.
# Renders directly with freetype + harfbuzz (no GTK, no cairo/pango, no
# dlopen renderer plugins).  Built-in `--run`/`--drun` list $PATH /
# desktop entries, or it reads candidate lines from stdin like dmenu.
#
# Deps: wayland-client, wayland-cursor, wayland-protocols (wlr-layer-
# shell, xdg-output), xkbcommon, freetype2, harfbuzz, fontconfig — all
# already in the sysroot for foot/sway.
#
# Produces /bin/tofi (+ tofi-run / tofi-drun wrappers).  sway config:
#   set $menu tofi-run | xargs swaymsg exec --
#   bindsym $mod+d exec $menu

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

TOFI_VERSION="0.9.1"
TOFI_URL="https://github.com/philj56/tofi/archive/refs/tags/v${TOFI_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/tofi-${TOFI_VERSION}"
TARBALL="$THIRD_PARTY/tofi-${TOFI_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/tofi_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-tofi] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading tofi ${TOFI_VERSION}"
        curl -fsSL -o "$TARBALL" "$TOFI_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
        # MakaOS patches — applied once, right after a fresh extraction.
        for p in "$REPO_ROOT"/scripts/patches/tofi/*.patch; do
            [ -e "$p" ] || continue
            log "applying $(basename "$p")"
            patch -p1 -d "$SRC_DIR" < "$p"
        done
    }
    # No scdoc on the host → drop the man-page subdir.
    if grep -q "subdir('doc')" "$SRC_DIR/meson.build" 2>/dev/null; then
        sed -i "s/^subdir('doc')/# subdir('doc') — no scdoc/" "$SRC_DIR/meson.build"
    fi
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
        -Db_pie=false \
        -Db_lto=false \
        -Dc_args="-D__STDC_ISO_10646__=201706L -Wno-error" \
        -Dc_link_args="-Wl,--allow-multiple-definition" \
        2>&1 | tee "$BUILD_DIR/tofi_meson.log" | tail -6
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" tofi 2>&1 | tee "$BUILD_DIR/tofi_ninja.log" | tail -6
    mkdir -p "$SYSROOT/usr/bin"
    install -m755 "$OUT_DIR/tofi" "$SYSROOT/usr/bin/tofi"
    # tofi-run / tofi-drun are symlinks to tofi in upstream; ship as such.
    ln -sf tofi "$SYSROOT/usr/bin/tofi-run"
    ln -sf tofi "$SYSROOT/usr/bin/tofi-drun"
    log "done — $(stat -c%s "$SYSROOT/usr/bin/tofi") bytes → $SYSROOT/usr/bin/tofi"
}

main() { fetch; configure; build_install; }
main "$@"
