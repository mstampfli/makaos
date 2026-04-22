#!/usr/bin/env bash
# ── MakaOS fcft port ──────────────────────────────────────────────────
#
# fcft is the font-compositor-for-terminals library used by foot
# (and nothing else we care about — it's the upstream author's
# abstraction over freetype/fontconfig/harfbuzz for terminal-style
# glyph lookup and grapheme shaping).  Required by foot.
#
# Deps: pixman, freetype2, fontconfig, tllist (header-only).
# Upstream has an optional harfbuzz dep — enabled since we have it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

FCFT_VERSION="3.3.1"
FCFT_URL="https://codeberg.org/dnkl/fcft/archive/${FCFT_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/fcft"
TARBALL="$THIRD_PARTY/fcft-${FCFT_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/fcft_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-fcft] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading fcft ${FCFT_VERSION}"
        curl -fsSL -o "$TARBALL" "$FCFT_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
    }
}

patch_meson() {
    local mb="$SRC_DIR/meson.build"
    grep -q MAKAOS_PATCHED "$mb" 2>/dev/null && { log "already patched"; return 0; }

    # Force static library — fcft's meson defaults to shared, our
    # sysroot is static-only.  Same pattern as libinput.
    python3 - "$mb" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
# Replace shared_library / library / both_libraries call.
s2 = re.sub(r"\blibrary\s*\(", "static_library(", s, count=0)
s2 = re.sub(r"\bshared_library\s*\(", "static_library(", s2)
s2 = "# MAKAOS_PATCHED — force static_library\n" + s2
open(p, 'w').write(s2)
PY
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
        -Ddefault_library=static \
        -Dtest-text-shaping=false \
        -Dgrapheme-shaping=enabled \
        -Drun-shaping=enabled \
        -Ddocs=disabled \
        -Dsvg-backend=nanosvg \
        2>&1 | tee "$BUILD_DIR/fcft_meson.log"
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" 2>&1 | tee "$BUILD_DIR/fcft_ninja.log"

    log "install → sysroot"
    DESTDIR="$SYSROOT" ninja -C "$OUT_DIR" install 2>&1 | tee "$BUILD_DIR/fcft_install.log"

    local lib="$SYSROOT/usr/lib/libfcft.a"
    [ -f "$lib" ] && log "done — $(stat -c%s "$lib") bytes → $lib" \
                   || { log "FAIL — libfcft.a not installed"; exit 1; }
}

main() { fetch; patch_meson; configure; build_install; }
main "$@"
