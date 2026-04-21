#!/usr/bin/env bash
# ── MakaOS libffi port (cross-gcc + sysroot) ──────────────────────────
#
# libffi normally autoconfigures and generates ffi.h + ffitarget.h
# from .in templates.  We bypass that — for the single x86_64 target
# we know exactly what those headers should contain, and they're
# stable across libffi 3.x.  Compile the unix64 backend directly.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBFFI_VERSION="3.4.6"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
LIBFFI_SRC="$THIRD_PARTY/libffi-${LIBFFI_VERSION}"
LIBFFI_TARBALL="$THIRD_PARTY/libffi-${LIBFFI_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-libffi] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CONFIG_HEADER="$REPO_ROOT/scripts/configs/libffi_config.h"

CFLAGS=(
    -O2
    -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-implicit-fallthrough
)

log() { printf '[port-libffi] %s\n' "$*" >&2; }

fetch_libffi() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$LIBFFI_TARBALL" ]; then
        log "downloading libffi ${LIBFFI_VERSION}"
        curl -fSL --retry 3 -o "$LIBFFI_TARBALL" "$LIBFFI_URL"
    fi
    if [ ! -d "$LIBFFI_SRC" ]; then
        log "extracting libffi"
        tar -xzf "$LIBFFI_TARBALL" -C "$THIRD_PARTY"
    fi
}

# Generate ffi.h + ffitarget.h ourselves from the .in templates.  For
# x86_64 they're trivial — just need TARGET=X86_64 and a few sed
# substitutions.  The .in templates use @VARIABLE@ tokens that
# autoconf would normally substitute.
install_headers() {
    log "generating + installing headers"
    mkdir -p "$SYSROOT/usr/include"
    # ffitarget.h is x86_64-specific, hand-copied from libffi's
    # src/x86/ffitarget.h.
    cp "$LIBFFI_SRC/src/x86/ffitarget.h" "$SYSROOT/usr/include/ffitarget.h"
    # ffi.h is generated from include/ffi.h.in by substituting:
    #   @TARGET@           → X86_64
    #   @HAVE_LONG_DOUBLE@ → 1
    #   @HAVE_LONG_DOUBLE_VARIANT@ → 0
    #   @FFI_EXEC_TRAMPOLINE_TABLE@ → 0
    #   @VERSION@          → 3.4.6
    sed \
      -e 's|@TARGET@|X86_64|g' \
      -e 's|@HAVE_LONG_DOUBLE@|1|g' \
      -e 's|@HAVE_LONG_DOUBLE_VARIANT@|0|g' \
      -e 's|@FFI_EXEC_TRAMPOLINE_TABLE@|0|g' \
      -e 's|@VERSION@|3.4.6|g' \
      "$LIBFFI_SRC/include/ffi.h.in" > "$SYSROOT/usr/include/ffi.h"
    # ffi_common.h is internal to the build, not installed, but our
    # build needs it findable.
}

build_lib() {
    local build_objs="$BUILD_DIR/libffi_objs"
    mkdir -p "$build_objs"

    # Copy our generated headers into the source tree where the
    # build expects to find them (config.h / fficonfig.h territory).
    cp "$SYSROOT/usr/include/ffi.h"       "$LIBFFI_SRC/include/ffi.h"
    cp "$SYSROOT/usr/include/ffitarget.h" "$LIBFFI_SRC/include/ffitarget.h"
    cp "$CONFIG_HEADER"                    "$LIBFFI_SRC/include/fficonfig.h"

    local includes=(
        -I "$LIBFFI_SRC/include"
        -I "$LIBFFI_SRC/src"
    )

    # Sources: top-level + x86 backend.  Skip ffiw64.c (Windows ABI),
    # skip closures.c special variants, skip dlmalloc (we use libc malloc).
    local srcs=(
        "src/prep_cif.c"
        "src/types.c"
        "src/raw_api.c"
        "src/java_raw_api.c"
        "src/closures.c"
        "src/x86/ffi64.c"
        "src/x86/ffiw64.c"   # EFI64 / GNUW64 ABI — never called on
                              # MakaOS but ffi64.c references its symbols
                              # unconditionally, so we need them linked.
    )
    local asm_srcs=(
        "src/x86/unix64.S"
        "src/x86/win64.S"     # accompanies ffiw64.c
    )

    log "compiling libffi sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$LIBFFI_SRC/$name"
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done
    for name in "${asm_srcs[@]}"; do
        local src="$LIBFFI_SRC/$name"
        local obj="$build_objs/$(basename "$name" .S).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libffi.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/libffi_objs" "$SYSROOT/usr/lib/libffi.a"
    fi
    if [ ! -f "$CONFIG_HEADER" ]; then
        log "ERROR: $CONFIG_HEADER not found"
        exit 1
    fi
    fetch_libffi
    install_headers
    build_lib
}

main "$@"
