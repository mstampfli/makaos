#!/usr/bin/env bash
# ── MakaOS freetype port (cross-gcc + sysroot) ────────────────────────
#
# TrueType/OpenType font rasterization.  freetype has its own build
# system — we bypass it and compile the top-level module aggregator
# files directly.  Each src/<mod>/<mod>.c pulls in the whole module.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FT_VERSION="2.13.3"
FT_URL="https://downloads.sourceforge.net/project/freetype/freetype2/${FT_VERSION}/freetype-${FT_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
FT_SRC="$THIRD_PARTY/freetype-${FT_VERSION}"
FT_TARBALL="$THIRD_PARTY/freetype-${FT_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-freetype] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CFLAGS=(
    -O2 -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -DFT2_BUILD_LIBRARY
    -DFT_CONFIG_OPTION_SYSTEM_ZLIB   # use sysroot zlib not freetype's copy
    -DHAVE_UNISTD_H                  # required by builds/unix/ftsystem.c
    -DHAVE_FCNTL_H
    # Note: FT_CONFIG_OPTION_USE_PNG / USE_HARFBUZZ / USE_BROTLI / MAC_FONTS
    # are gated by `defined()` in the source, not value.  Leaving them
    # undefined is correct; explicitly `-DFOO=0` would trigger the
    # enabled path (C preprocessor `defined()` trap).
)

log() { printf '[port-freetype] %s\n' "$*" >&2; }

fetch_ft() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$FT_TARBALL" ]; then
        log "downloading freetype ${FT_VERSION}"
        curl -fSL --retry 3 -o "$FT_TARBALL" "$FT_URL"
    fi
    if [ ! -d "$FT_SRC" ]; then
        log "extracting freetype"
        tar -xJf "$FT_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/freetype2"
    mkdir -p "$SYSROOT/usr/include/freetype2"
    cp -rT "$FT_SRC/include/" "$SYSROOT/usr/include/freetype2/"
}

build_lib() {
    local build_objs="$BUILD_DIR/ft_objs"
    mkdir -p "$build_objs"

    local includes=(
        -I "$FT_SRC/include"
        -I "$FT_SRC/src/base"
    )

    # Each listed .c is a module aggregator that #includes every
    # sub-file of the module — a deliberate design of freetype's
    # build system.  We just compile each aggregator once.
    # Use the Unix ftsystem.c (mmap-backed) not the generic stdio one.
    # The generic version wraps fopen/fread/fseek for every stream I/O,
    # which turns each of freetype's ~N glyph-index lookups into a
    # syscall — observed ~9000 syscalls per font open, ~75 s of wall
    # time on a 343 KB TTF.  The Unix ftsystem.c mmap()s the font at
    # FT_Stream_Open time, so lookups become pointer derefs.
    local srcs=(
        "builds/unix/ftsystem.c"
        "src/base/ftinit.c"
        "src/base/ftdebug.c"
        "src/base/ftbase.c"
        "src/base/ftbbox.c"
        "src/base/ftbdf.c"
        "src/base/ftbitmap.c"
        "src/base/ftcid.c"
        "src/base/ftfstype.c"
        "src/base/ftgasp.c"
        "src/base/ftglyph.c"
        "src/base/ftgxval.c"
        "src/base/ftmm.c"
        "src/base/ftotval.c"
        "src/base/ftpatent.c"
        "src/base/ftpfr.c"
        "src/base/ftstroke.c"
        "src/base/ftsynth.c"
        "src/base/fttype1.c"
        "src/base/ftwinfnt.c"

        # Font format modules
        "src/truetype/truetype.c"
        "src/type1/type1.c"
        "src/cff/cff.c"
        "src/cid/type1cid.c"
        "src/pfr/pfr.c"
        "src/type42/type42.c"
        "src/winfonts/winfnt.c"
        "src/pcf/pcf.c"
        "src/bdf/bdf.c"

        # Rasterizers
        "src/sfnt/sfnt.c"
        "src/smooth/smooth.c"
        "src/raster/raster.c"
        "src/svg/svg.c"

        # Auxiliary
        "src/psaux/psaux.c"
        "src/psnames/psmodule.c"
        "src/pshinter/pshinter.c"
        "src/autofit/autofit.c"

        # Cache + compression
        "src/cache/ftcache.c"
        "src/gzip/ftgzip.c"
        "src/lzw/ftlzw.c"
        "src/bzip2/ftbzip2.c"

        # SDF (signed distance field) is new but small
        "src/sdf/sdf.c"
    )

    log "compiling ${#srcs[@]} freetype sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$FT_SRC/$name"
        [ -f "$src" ] || { log "missing $name (skip)"; continue; }
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libfreetype.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/ft_objs" "$SYSROOT/usr/lib/libfreetype.a"
    fi
    fetch_ft
    install_headers
    build_lib
}

main "$@"
