#!/usr/bin/env bash
# ── MakaOS libutf8proc port ────────────────────────────────────────────
#
# libutf8proc is JuliaStrings' standalone C library for Unicode
# normalization, case folding, grapheme segmentation, and boundary
# analysis.  Required by fcft (for run-shaping / grapheme clustering)
# which is required by foot.  Single source file (utf8proc.c + data
# tables), no external deps — compile + ar directly, skip meson/cmake.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="2.9.0"
URL="https://github.com/JuliaStrings/utf8proc/releases/download/v${VERSION}/utf8proc-${VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/utf8proc-${VERSION}"
TARBALL="$THIRD_PARTY/utf8proc-${VERSION}.tar.gz"
OBJS_DIR="$BUILD_DIR/utf8proc_objs"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

CFLAGS=(
    --sysroot="$SYSROOT"
    -nostdinc
    -isystem "$SYSROOT/usr/include"
    -O2
    -DUTF8PROC_STATIC
)

log() { printf '[port-libutf8proc] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading utf8proc ${VERSION}"
        curl -fsSL -o "$TARBALL" "$URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
    }
}

build_install() {
    mkdir -p "$OBJS_DIR"
    log "compiling utf8proc.c"
    "$CROSS_CC" "${CFLAGS[@]}" -I "$SRC_DIR" -c "$SRC_DIR/utf8proc.c" \
        -o "$OBJS_DIR/utf8proc.o"
    local lib="$SYSROOT/usr/lib/libutf8proc.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "$OBJS_DIR/utf8proc.o"

    /usr/bin/install -Dm644 "$SRC_DIR/utf8proc.h" "$SYSROOT/usr/include/utf8proc.h"

    cat > "$SYSROOT/usr/lib/pkgconfig/libutf8proc.pc" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
Name: libutf8proc
Description: Unicode processing library
Version: ${VERSION}
Libs: -L\${libdir} -lutf8proc
Cflags: -I\${includedir} -DUTF8PROC_STATIC
EOF
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() { fetch; build_install; }
main "$@"
