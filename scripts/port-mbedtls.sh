#!/usr/bin/env bash
# ── MakaOS mbedTLS port (cross-gcc + sysroot) ──────────────────────────
#
# Fetches pinned mbedTLS, compiles it with x86_64-pc-makaos-gcc, and
# installs libmbedtls.a + headers into the sysroot.  Zero per-port
# shim headers, zero bespoke include-path soup — the cross-gcc knows
# its own target triple and sysroot.
#
# No mbedTLS sources are committed to this repo.

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

# Cross-compiler.  Falls back to host gcc with a loud warning if the
# toolchain isn't built — only useful during very early dev.
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-mbedtls] WARNING: cross-toolchain missing — run scripts/build-toolchain.sh" >&2
    echo "[port-mbedtls] falling back to host gcc (legacy path, not production)" >&2
    CROSS_CC="${CC:-gcc}"
    CROSS_AR="${AR:-ar}"
fi

# With the cross-gcc, the target triple carries:
#   --sysroot, -nostdinc-equivalent, ELF64, static-by-default,
#   freestanding defaults, __makaos__ + __unix__ + __ELF__ predefines.
# So our flag list is just "tell it about the config header + -O2".
CFLAGS=(
    -O2
    -fPIC        # needed by a few PSA tests; harmless for static archives
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

    log "compiling ${#srcs[@]} mbedtls sources with $CROSS_CC"
    local objs=()
    for src in "${srcs[@]}"; do
        local base; base="$(basename "$src" .c)"
        local obj="$build_objs/${base}.o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ] || [ "$CONFIG_HEADER" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libmbedtls.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
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
