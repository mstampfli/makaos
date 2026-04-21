#!/usr/bin/env bash
# ── MakaOS wayland-protocols port ──────────────────────────────────────
#
# Pure data — no build, no cross-compile.  Install the canonical protocol
# XML tree into the sysroot where wlroots and Hyprland look for it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WP_VERSION="1.38"
WP_URL="https://gitlab.freedesktop.org/wayland/wayland-protocols/-/archive/${WP_VERSION}/wayland-protocols-${WP_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
WP_SRC="$THIRD_PARTY/wayland-protocols-${WP_VERSION}"
WP_TARBALL="$THIRD_PARTY/wayland-protocols-${WP_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
WP_SHARE="$SYSROOT/usr/share/wayland-protocols"

log() { printf '[port-wp] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$WP_TARBALL" ]; then
        log "downloading wayland-protocols ${WP_VERSION}"
        curl -fsSL -o "$WP_TARBALL" "$WP_URL"
    fi
    if [ ! -d "$WP_SRC" ]; then
        log "extracting wayland-protocols"
        tar -xzf "$WP_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_xml() {
    mkdir -p "$WP_SHARE"
    log "installing protocol XML → $WP_SHARE/{stable,staging,unstable}"
    # Mirror the upstream directory layout — wlroots/meson find protocols
    # via pkg-config (pkgdatadir=share/wayland-protocols) and then
    # stable/xdg-shell/xdg-shell.xml etc.
    cp -r "$WP_SRC/stable"   "$WP_SHARE/"
    cp -r "$WP_SRC/staging"  "$WP_SHARE/"
    cp -r "$WP_SRC/unstable" "$WP_SHARE/"
    local count=$(find "$WP_SHARE" -name "*.xml" | wc -l)
    log "installed $count protocol XML files"
}

install_pkgconfig() {
    # wlroots queries pkg-config for wayland-protocols to find the XML
    # install path.  Write a minimal .pc file into the sysroot so
    # cross-builds can resolve it.
    mkdir -p "$SYSROOT/usr/lib/pkgconfig"
    cat > "$SYSROOT/usr/lib/pkgconfig/wayland-protocols.pc" <<EOF
prefix=/usr
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland-protocols

Name: Wayland Protocols
Description: Wayland protocol XML files
Version: ${WP_VERSION}
EOF
    log "installed wayland-protocols.pc"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior install"
        rm -rf "$WP_SHARE"
    fi
    fetch
    install_xml
    install_pkgconfig
    log "done"
}

main "$@"
