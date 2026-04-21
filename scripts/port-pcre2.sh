#!/usr/bin/env bash
# ── MakaOS libpcre2-8 port ─────────────────────────────────────────────
#
# Regex library.  8-bit code unit only (what sway uses).  Hand-rolled
# build — skip autotools.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PCRE_VERSION="10.44"
PCRE_URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE_VERSION}/pcre2-${PCRE_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
PCRE_SRC="$THIRD_PARTY/pcre2-${PCRE_VERSION}"
PCRE_TARBALL="$THIRD_PARTY/pcre2-${PCRE_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-pcre2] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$PCRE_TARBALL" ] || { log "downloading"; curl -fsSL -o "$PCRE_TARBALL" "$PCRE_URL"; }
    [ -d "$PCRE_SRC" ] || tar -xzf "$PCRE_TARBALL" -C "$THIRD_PARTY"

    # Generic config.h is provided by upstream; pcre2.h template needs
    # the @...@ placeholders resolved.  Use pcre2.h.generic for both.
    cp "$PCRE_SRC/src/config.h.generic"       "$PCRE_SRC/src/config.h"
    cp "$PCRE_SRC/src/pcre2.h.generic"        "$PCRE_SRC/src/pcre2.h"
    cp "$PCRE_SRC/src/pcre2_chartables.c.dist" "$PCRE_SRC/src/pcre2_chartables.c"
}

build_lib() {
    local objdir="$BUILD_DIR/pcre2_objs"
    mkdir -p "$objdir"

    local cflags=(
        -O2 -fPIC -std=gnu11
        -Wall
        -Wno-unused-parameter
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-unused-const-variable
        -Wno-implicit-function-declaration
        -Wno-sign-compare
        -DPCRE2_CODE_UNIT_WIDTH=8
        -DHAVE_CONFIG_H
        -DPCRE2_STATIC
        --sysroot="$SYSROOT"
        -nostdinc
        -isystem "$SYSROOT/usr/include"
        -I "$PCRE_SRC/src"
    )

    local srcs=(
        pcre2_auto_possess.c
        pcre2_chartables.c
        pcre2_chkdint.c
        pcre2_compile.c
        pcre2_config.c
        pcre2_context.c
        pcre2_convert.c
        pcre2_dfa_match.c
        pcre2_error.c
        pcre2_extuni.c
        pcre2_find_bracket.c
        pcre2_jit_compile.c
        pcre2_maketables.c
        pcre2_match.c
        pcre2_match_data.c
        pcre2_newline.c
        pcre2_ord2utf.c
        pcre2_pattern_info.c
        pcre2_script_run.c
        pcre2_serialize.c
        pcre2_string_utils.c
        pcre2_study.c
        pcre2_substitute.c
        pcre2_substring.c
        pcre2_tables.c
        pcre2_ucd.c
        pcre2_valid_utf.c
        pcre2_xclass.c
    )

    local objs=()
    for src in "${srcs[@]}"; do
        local o="$objdir/${src%.c}.o"
        objs+=("$o")
        if [ "$PCRE_SRC/src/$src" -nt "$o" ]; then
            "$CROSS_CC" "${cflags[@]}" -c "$PCRE_SRC/src/$src" -o "$o"
        fi
    done

    rm -f "$SYSROOT/usr/lib/libpcre2-8.a"
    "$CROSS_AR" rcs "$SYSROOT/usr/lib/libpcre2-8.a" "${objs[@]}"
    log "libpcre2-8.a: $(stat -c%s "$SYSROOT/usr/lib/libpcre2-8.a") bytes"
}

install() {
    cp "$PCRE_SRC/src/pcre2.h"       "$SYSROOT/usr/include/pcre2.h"
    cp "$PCRE_SRC/src/pcre2posix.h"  "$SYSROOT/usr/include/pcre2posix.h" 2>/dev/null || true

    mkdir -p "$SYSROOT/usr/lib/pkgconfig"
    cat > "$SYSROOT/usr/lib/pkgconfig/libpcre2-8.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libpcre2-8
Description: PCRE2 regex library (8-bit)
Version: ${PCRE_VERSION}
Libs: -L\${libdir} -lpcre2-8
Cflags: -I\${includedir} -DPCRE2_CODE_UNIT_WIDTH=8
EOF
}

main() {
    [ "${FORCE:-0}" = "1" ] && rm -rf "$BUILD_DIR/pcre2_objs" "$SYSROOT/usr/lib/libpcre2-8.a"
    fetch
    build_lib
    install
    log "done"
}
main "$@"
