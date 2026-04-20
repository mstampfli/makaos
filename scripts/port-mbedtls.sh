#!/usr/bin/env bash
# ── MakaOS mbedTLS port tool ──────────────────────────────────────────
#
# Fetches a pinned mbedTLS release, applies the MakaOS build config, and
# compiles the library against the userland freestanding toolchain.  The
# output is a static archive linked into any userland binary that asks
# for TLS (initially just /bin/http_get, later anything else that wants
# HTTPS / TLS 1.3).
#
# First invocation downloads + extracts + compiles (~2-3 minutes).  Later
# invocations reuse the extracted tree and only recompile if the library
# is missing.  Pass FORCE=1 to rebuild from scratch.
#
# No mbedTLS sources are committed to this repo — only the config header
# and glue layer.  Upstream code is cached in build/third_party/mbedtls.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MBEDTLS_VERSION="3.6.2"                       # LTS branch, has stable TLS 1.3
MBEDTLS_SHA256="8b660f28d92fff5ae3f7f8c3d1d301d4fe68cb5b8a3a4a10b35a0c90af8f7f7e"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/mbedtls-${MBEDTLS_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
MBEDTLS_SRC="$THIRD_PARTY/mbedtls-${MBEDTLS_VERSION}"
MBEDTLS_TARBALL="$THIRD_PARTY/mbedtls-${MBEDTLS_VERSION}.tar.gz"

LIBS_DIR="$REPO_ROOT/userland/libs/mbedtls"
INCLUDE_DIR="$LIBS_DIR/include"
LIB_OUT="$LIBS_DIR/libmbedtls.a"

CONFIG_HEADER="$LIBS_DIR/mbedtls_config.h"    # MakaOS-specific config

CC="${CC:-gcc}"
AR="${AR:-ar}"

# Must match USER_CFLAGS in build.sh — freestanding userland.  We add
# -nostdinc to keep the host's /usr/include (glibc) away from mbedTLS:
# it fights our libc.h over fd_set, pselect, imaxdiv, etc.
USER_CFLAGS=(
    -ffreestanding -m64 -mno-red-zone
    -fno-pie -fno-pic -fno-plt
    -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -nostdinc
    -O2                                       # TLS is hot — let the compiler optimise
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-unused-but-set-variable
    -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
)

# All standard-header lookups resolve inside shim/ — which forwards
# everything to our libc.h.  No glibc leakage, no gcc-builtin stddef.h
# (its `typedef long int size_t` fights libc.h's `typedef uint64_t
# size_t`, even though both are 64-bit on x86_64, per C's strict-type
# rule).
USER_INCLUDES=(
    -I "$LIBS_DIR/shim"
    -I "$REPO_ROOT/userland/libc"
    -I "$REPO_ROOT/userland/include"
    -I "$LIBS_DIR"                            # mbedtls_config.h
    -I "$MBEDTLS_SRC/include"
    -I "$MBEDTLS_SRC/library"
)

log() { printf '[port-mbedtls] %s\n' "$*" >&2; }

# ── 1. Fetch & extract the upstream tarball ─────────────────────────
fetch_mbedtls() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$MBEDTLS_TARBALL" ]; then
        log "downloading mbedtls ${MBEDTLS_VERSION}"
        curl -fSL --retry 3 -o "$MBEDTLS_TARBALL" "$MBEDTLS_URL"
    fi
    if [ ! -d "$MBEDTLS_SRC" ]; then
        log "extracting mbedtls"
        tar -xzf "$MBEDTLS_TARBALL" -C "$THIRD_PARTY"
        # Upstream uses "mbedtls-mbedtls-<ver>" as the top-level dir.
        if [ -d "$THIRD_PARTY/mbedtls-mbedtls-${MBEDTLS_VERSION}" ]; then
            mv "$THIRD_PARTY/mbedtls-mbedtls-${MBEDTLS_VERSION}" "$MBEDTLS_SRC"
        fi
    fi
}

# ── 2. Compile library .c files into libmbedtls.a ───────────────────
build_mbedtls() {
    local build_objs="$BUILD_DIR/mbedtls_objs"
    mkdir -p "$build_objs" "$LIBS_DIR"

    # mbedTLS lives in library/*.c — ~130 source files (full build).
    # We compile everything including psa_*.c — TLS 1.3 in mbedTLS 3.x
    # routes through the PSA Crypto subsystem and won't compile without
    # those sources.
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
        # Only recompile if stale.
        if [ "$src" -nt "$obj" ] || [ "$CONFIG_HEADER" -nt "$obj" ]; then
            "$CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
                -c "$src" -o "$obj"
        fi
    done

    log "archiving → $LIB_OUT"
    rm -f "$LIB_OUT"
    "$AR" rcs "$LIB_OUT" "${objs[@]}"
    log "done — $(stat -c%s "$LIB_OUT") bytes"
}

# ── 3. Expose headers under userland/libs/mbedtls/include/mbedtls/ ──
export_headers() {
    rm -rf "$INCLUDE_DIR"
    mkdir -p "$INCLUDE_DIR"
    cp -R "$MBEDTLS_SRC/include/"* "$INCLUDE_DIR/"
    log "headers exported → $INCLUDE_DIR"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/mbedtls_objs" "$LIB_OUT" "$INCLUDE_DIR"
    fi

    if [ ! -f "$CONFIG_HEADER" ]; then
        log "ERROR: $CONFIG_HEADER not found — write it before running this script"
        exit 1
    fi

    fetch_mbedtls
    export_headers
    build_mbedtls
}

main "$@"
