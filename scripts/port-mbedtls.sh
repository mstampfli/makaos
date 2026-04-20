#!/usr/bin/env bash
# ── MakaOS mbedTLS port (sysroot-based) ────────────────────────────────
#
# Fetches a pinned mbedTLS release and compiles it against the MakaOS
# sysroot built by build.sh.  Produces:
#   $SYSROOT/usr/include/mbedtls/*.h   (public headers)
#   $SYSROOT/usr/include/psa/*.h
#   $SYSROOT/usr/lib/libmbedtls.a      (static archive)
#
# No per-library shim dir, no source in the repo.  Upstream tarball is
# cached in build/third_party/.  Re-run with FORCE=1 to rebuild.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBEDTLS_VERSION="3.6.2"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/mbedtls-${MBEDTLS_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
MBEDTLS_SRC="$THIRD_PARTY/mbedtls-${MBEDTLS_VERSION}"
MBEDTLS_TARBALL="$THIRD_PARTY/mbedtls-${MBEDTLS_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CONFIG_HEADER="$REPO_ROOT/scripts/configs/mbedtls_config.h"

CC="${CC:-gcc}"
AR="${AR:-ar}"

# Must match USER_CFLAGS in build.sh — sysroot-based freestanding userland.
USER_CFLAGS=(
    -ffreestanding -m64 -mno-red-zone
    -fno-pie -fno-pic -fno-plt
    -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    --sysroot="$SYSROOT"
    -nostdinc
    -isystem "$SYSROOT/usr/include"
    -O2
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-unused-but-set-variable
    -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
    -I "$(dirname "$CONFIG_HEADER")"
    -I "$MBEDTLS_SRC/include"
    -I "$MBEDTLS_SRC/library"
)

log() { printf '[port-mbedtls] %s\n' "$*" >&2; }

fetch_mbedtls() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$MBEDTLS_TARBALL" ]; then
        log "downloading mbedtls ${MBEDTLS_VERSION}"
        curl -fSL --retry 3 -o "$MBEDTLS_TARBALL" "$MBEDTLS_URL"
    fi
    if [ ! -d "$MBEDTLS_SRC" ]; then
        log "extracting mbedtls"
        tar -xzf "$MBEDTLS_TARBALL" -C "$THIRD_PARTY"
        if [ -d "$THIRD_PARTY/mbedtls-mbedtls-${MBEDTLS_VERSION}" ]; then
            mv "$THIRD_PARTY/mbedtls-mbedtls-${MBEDTLS_VERSION}" "$MBEDTLS_SRC"
        fi
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include"
    mkdir -p "$SYSROOT/usr/include"
    cp -rT "$MBEDTLS_SRC/include" "$SYSROOT/usr/include"
}

build_lib() {
    local build_objs="$BUILD_DIR/mbedtls_objs"
    mkdir -p "$build_objs"

    local srcs=()
    while IFS= read -r -d '' f; do
        srcs+=("$f")
    done < <(find "$MBEDTLS_SRC/library" -maxdepth 1 -name '*.c' -print0)

    log "compiling ${#srcs[@]} mbedtls sources"
    local objs=()
    for src in "${srcs[@]}"; do
        local base; base="$(basename "$src" .c)"
        local obj="$build_objs/${base}.o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ] || [ "$CONFIG_HEADER" -nt "$obj" ]; then
            "$CC" "${USER_CFLAGS[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libmbedtls.a"
    rm -f "$lib"
    "$AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/mbedtls_objs" "$SYSROOT/usr/lib/libmbedtls.a"
    fi
    if [ ! -f "$CONFIG_HEADER" ]; then
        log "ERROR: $CONFIG_HEADER not found"
        exit 1
    fi
    if [ ! -d "$SYSROOT/usr/include" ]; then
        log "ERROR: sysroot not populated ($SYSROOT/usr/include missing)"
        log "       Run build.sh first — it builds the sysroot before ports."
        exit 1
    fi

    fetch_mbedtls
    install_headers
    build_lib
}

main "$@"
