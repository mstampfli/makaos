#!/usr/bin/env bash
# ── MakaOS fontconfig port (cross-gcc + sysroot) ──────────────────────
#
# Font discovery library.  Depends on freetype + libexpat (both ported
# earlier).  Minimal build — we don't wire in the system font cache
# directory as something dynamic; clients pass explicit font paths or
# rely on the sysroot-baked config.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FC_VERSION="2.15.0"
FC_URL="https://www.freedesktop.org/software/fontconfig/release/fontconfig-${FC_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
FC_SRC="$THIRD_PARTY/fontconfig-${FC_VERSION}"
FC_TARBALL="$THIRD_PARTY/fontconfig-${FC_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-fc] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CONFIG_HEADER="$REPO_ROOT/scripts/configs/fontconfig_config.h"

CFLAGS=(
    -O2 -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-pointer-sign
    -DHAVE_CONFIG_H
    -I "$SYSROOT/usr/include/freetype2"
)

log() { printf '[port-fc] %s\n' "$*" >&2; }

fetch_fc() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$FC_TARBALL" ]; then
        log "downloading fontconfig ${FC_VERSION}"
        curl -fSL --retry 3 -o "$FC_TARBALL" "$FC_URL"
    fi
    if [ ! -d "$FC_SRC" ]; then
        log "extracting fontconfig"
        tar -xJf "$FC_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/fontconfig"
    mkdir -p "$SYSROOT/usr/include/fontconfig"
    cp "$FC_SRC/fontconfig/"*.h "$SYSROOT/usr/include/fontconfig/"
}

build_lib() {
    local build_objs="$BUILD_DIR/fc_objs"
    mkdir -p "$build_objs"

    cp "$CONFIG_HEADER" "$FC_SRC/config.h"

    # Generate fcalias.h/fcaliastail.h + fcftalias.h/fcftaliastail.h
    # via the makealias shell script (normally invoked from Makefile.am
    # / meson.build).  Lists of public symbols come from the public
    # headers; the script rewrites declarations with hidden visibility.
    if [ ! -f "$FC_SRC/src/fcalias.h" ]; then
        (cd "$FC_SRC/src" && \
         sh ./makealias "$FC_SRC/src" fcalias.h fcaliastail.h \
            fontconfig.h fcprivate.h fcdeprecate.h)
    fi
    # makealias's fcftalias.h path is broken when the freetype public
    # header has no FcFT*-prefixed entry points (which is our case —
    # modern fontconfig moved them elsewhere).  It generates an
    # unmatched fcftaliastail.h and no fcftalias.h.  Hidden-visibility
    # aliasing is purely an optimization; empty both files so the
    # #include directives become no-ops.
    : > "$FC_SRC/src/fcftalias.h"
    : > "$FC_SRC/src/fcftaliastail.h"

    local includes=(
        -I "$FC_SRC"
        -I "$FC_SRC/src"
        -I "$FC_SRC/fontconfig"
    )

    # Fontconfig sources — single flat directory under src/
    # fccache.c + fcatomic.c pull in `utimes`, `struct flock`, `timercmp`
    # that our libc doesn't yet ship.  Skip — fontconfig without the
    # on-disk cache still functions (scans fonts on each init, slower
    # startup but correct).  Revisit when libc gains these.
    local srcs=(
        "src/fccfg.c"
        "src/fccharset.c"
        "src/fccompat.c"
        "src/fcdbg.c"
        "src/fcdefault.c"
        "src/fcdir.c"
        "src/fcformat.c"
        "src/fcfreetype.c"
        "src/fcfs.c"
        "src/fcptrlist.c"
        "src/fchash.c"
        "src/fcinit.c"
        "src/fclang.c"
        "src/fclist.c"
        "src/fcmatch.c"
        "src/fcmatrix.c"
        "src/fcname.c"
        "src/fcobjs.c"
        "src/fcpat.c"
        "src/fcrange.c"
        "src/fcserialize.c"
        "src/fcstat.c"
        "src/fcstr.c"
        "src/fcweight.c"
        "src/fcxml.c"
        "src/ftglue.c"
    )

    log "compiling ${#srcs[@]} fontconfig sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$FC_SRC/$name"
        [ -f "$src" ] || { log "missing $name (skip)"; continue; }
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libfontconfig.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/fc_objs" "$SYSROOT/usr/lib/libfontconfig.a"
    fi
    fetch_fc
    install_headers
    build_lib
}

main "$@"
