#!/usr/bin/env bash
# ── MakaOS libpng port (cross-gcc + sysroot) ──────────────────────────
#
# PNG codec on top of the zlib port.  Enables cairo's PNG surface
# (swaybar image fallback) and any future gdk-pixbuf/wallpaper work.
# Compiled manually like pixman — the autoconf only produces
# pnglibconf.h, which upstream ships prebuilt.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PNG_VERSION="1.6.44"
PNG_URL="https://github.com/pnggroup/libpng/archive/refs/tags/v${PNG_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
PNG_SRC="$THIRD_PARTY/libpng-${PNG_VERSION}"
PNG_TARBALL="$THIRD_PARTY/libpng-${PNG_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-libpng] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CFLAGS=(
    -O2 -fPIC -Wall
    -Wno-unused-parameter
    --sysroot="$SYSROOT"
    -nostdinc
    -isystem "$SYSROOT/usr/include"
    # Intel SIMD paths need runtime dispatch glue; the C filters are
    # plenty for titlebar/icon-sized images.
    -DPNG_INTEL_SSE_OPT=0
)

log() { printf '[port-libpng] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$PNG_TARBALL" ] || {
        log "downloading libpng ${PNG_VERSION}"
        curl -fSL --retry 3 -o "$PNG_TARBALL" "$PNG_URL"
    }
    [ -d "$PNG_SRC" ] || {
        log "extracting libpng"
        tar -xzf "$PNG_TARBALL" -C "$THIRD_PARTY"
    }
}

build() {
    local build_objs="$BUILD_DIR/libpng_objs"
    mkdir -p "$build_objs"

    # pnglibconf.h: upstream ships the default configuration prebuilt.
    [ -f "$PNG_SRC/pnglibconf.h" ] || \
        cp "$PNG_SRC/scripts/pnglibconf.h.prebuilt" "$PNG_SRC/pnglibconf.h"

    local srcs=(
        png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c
        pngrio.c pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c
        pngwrite.c pngwtran.c pngwutil.c
    )

    log "compiling ${#srcs[@]} libpng sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local obj="$build_objs/${name%.c}.o"
        objs+=("$obj")
        if [ "$PNG_SRC/$name" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" -c "$PNG_SRC/$name" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libpng16.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"

    log "installing headers"
    mkdir -p "$SYSROOT/usr/include/libpng16"
    cp "$PNG_SRC/png.h" "$PNG_SRC/pngconf.h" "$PNG_SRC/pnglibconf.h" \
       "$SYSROOT/usr/include/libpng16/"
    for h in png.h pngconf.h pnglibconf.h; do
        ln -sf "libpng16/$h" "$SYSROOT/usr/include/$h"
    done

    mkdir -p "$SYSROOT/usr/lib/pkgconfig"
    cat > "$SYSROOT/usr/lib/pkgconfig/libpng16.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libpng
Description: Loads and saves PNG files
Version: ${PNG_VERSION}
Requires: zlib
Libs: -L\${libdir} -lpng16
Cflags: -I\${includedir}/libpng16
EOF
    ln -sf libpng16.pc "$SYSROOT/usr/lib/pkgconfig/libpng.pc"

    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        rm -rf "$BUILD_DIR/libpng_objs" "$SYSROOT/usr/lib/libpng16.a"
    fi
    fetch
    build
}
main "$@"
