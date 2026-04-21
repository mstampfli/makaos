#!/usr/bin/env bash
# ── MakaOS wayland port ────────────────────────────────────────────────
#
# Two-stage build:
#
#   1. wayland-scanner (HOST tool)
#      Compiled with the host gcc because it runs on the dev machine
#      during the cross-build of libwayland.  Takes protocol XML and
#      emits C dispatch code.  Produces build/host-tools/bin/wayland-scanner.
#
#   2. libwayland-{client,server} (TARGET libraries, cross-compiled)
#      Compiled with the MakaOS cross-gcc against our sysroot.  Depends
#      on libffi (Tier 2, done) + libexpat (Tier 2, done) + the generated
#      protocol dispatch tables from stage 1.
#
# Install:
#   build/sysroot/usr/lib/libwayland-client.a
#   build/sysroot/usr/lib/libwayland-server.a
#   build/sysroot/usr/include/wayland-{client,server,util,version}*.h
#   build/sysroot/usr/share/wayland/wayland.xml

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WL_VERSION="1.23.1"
WL_URL="https://gitlab.freedesktop.org/wayland/wayland/-/releases/${WL_VERSION}/downloads/wayland-${WL_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
WL_SRC="$THIRD_PARTY/wayland-${WL_VERSION}"
WL_TARBALL="$THIRD_PARTY/wayland-${WL_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
HOST_TOOLS="$BUILD_DIR/host-tools"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

if [ ! -x "$CROSS_CC" ]; then
    echo "[port-wayland] FATAL: cross-toolchain missing" >&2
    exit 1
fi

log() { printf '[port-wayland] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$WL_TARBALL" ]; then
        log "downloading wayland ${WL_VERSION}"
        curl -fsSL -o "$WL_TARBALL" "$WL_URL"
    fi
    if [ ! -d "$WL_SRC" ]; then
        log "extracting wayland"
        tar -xJf "$WL_TARBALL" -C "$THIRD_PARTY"
    fi
    # Synthesize wayland-version.h from .in — meson normally does this.
    if [ ! -f "$WL_SRC/src/wayland-version.h" ]; then
        sed -e 's/@WAYLAND_VERSION_MAJOR@/1/' \
            -e 's/@WAYLAND_VERSION_MINOR@/23/' \
            -e 's/@WAYLAND_VERSION_MICRO@/1/' \
            -e 's/@WAYLAND_VERSION@/1.23.1/' \
            "$WL_SRC/src/wayland-version.h.in" > "$WL_SRC/src/wayland-version.h"
    fi
    # Synthesize config.h — wayland sources check a handful of HAVE_*
    # defines.  All of ours are satisfied by POSIX 2008.
    cat > "$WL_SRC/config.h" <<'CFG'
/* MakaOS port config.h for wayland 1.23.1
 *
 * Wayland sources use #ifdef HAVE_X (not #if) for most of these,
 * so "no" means UNDEFINED, not #define 0.  Things we DON'T have
 * must be absent from this file entirely. */
#pragma once
#define WAYLAND_VERSION           "1.23.1"
CFG
}

# ── Stage 1: build wayland-scanner on the host ──────────────────────
build_scanner() {
    mkdir -p "$HOST_TOOLS/bin"
    log "compiling wayland-scanner (host gcc + system libexpat)"
    gcc -O2 -std=gnu11 -Wall \
        -I "$WL_SRC" -I "$WL_SRC/src" \
        "$WL_SRC/src/scanner.c" "$WL_SRC/src/wayland-util.c" \
        -lexpat \
        -o "$HOST_TOOLS/bin/wayland-scanner"
    log "wayland-scanner installed at $HOST_TOOLS/bin/wayland-scanner"
}

# ── Generate the core wayland protocol dispatch code ────────────────
generate_core_proto() {
    local gen_dir="$WL_SRC/generated"
    mkdir -p "$gen_dir"
    local scanner="$HOST_TOOLS/bin/wayland-scanner"
    local xml="$WL_SRC/protocol/wayland.xml"
    log "generating wayland-{client,server}-protocol.[ch] from wayland.xml"
    "$scanner" client-header "$xml" "$gen_dir/wayland-client-protocol.h"
    "$scanner" server-header "$xml" "$gen_dir/wayland-server-protocol.h"
    "$scanner" private-code  "$xml" "$gen_dir/wayland-protocol.c"
}

# ── Stage 2: cross-compile libwayland-client + libwayland-server ─────
build_target_libs() {
    local objdir="$BUILD_DIR/wayland_objs"
    mkdir -p "$objdir"

    local cflags=(
        -O2 -fPIC -std=gnu11
        -Wall
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-missing-field-initializers
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-stringop-truncation
        -Wno-format-truncation
        -Wno-strict-aliasing
        -DHAVE_CONFIG_H
        -I "$WL_SRC"
        -I "$WL_SRC/src"
        -I "$WL_SRC/generated"
        -I "$SYSROOT/usr/include"
    )

    local common_src=(
        src/connection.c
        src/wayland-os.c
        src/wayland-util.c
        src/event-loop.c
        generated/wayland-protocol.c
    )
    local client_src=(
        src/wayland-client.c
    )
    local server_src=(
        src/wayland-server.c
        src/wayland-shm.c
    )

    compile_group() {
        local group="$1"; shift
        local objs=()
        for src in "$@"; do
            local o="$objdir/${group}_$(basename "$src" .c).o"
            objs+=("$o")
            if [ "$WL_SRC/$src" -nt "$o" ]; then
                "$CROSS_CC" "${cflags[@]}" -c "$WL_SRC/$src" -o "$o" \
                    || { log "FAIL compiling $src"; return 1; }
            fi
        done
        printf '%s\n' "${objs[@]}"
    }

    log "cross-compiling shared objects"
    mapfile -t common_objs < <(compile_group common "${common_src[@]}")

    log "cross-compiling client library"
    mapfile -t client_objs < <(compile_group client "${client_src[@]}")

    log "cross-compiling server library"
    mapfile -t server_objs < <(compile_group server "${server_src[@]}")

    rm -f "$SYSROOT/usr/lib/libwayland-client.a" \
          "$SYSROOT/usr/lib/libwayland-server.a"
    "$CROSS_AR" rcs "$SYSROOT/usr/lib/libwayland-client.a" \
        "${client_objs[@]}" "${common_objs[@]}"
    "$CROSS_AR" rcs "$SYSROOT/usr/lib/libwayland-server.a" \
        "${server_objs[@]}" "${common_objs[@]}"
    log "libwayland-client.a: $(stat -c%s "$SYSROOT/usr/lib/libwayland-client.a") bytes"
    log "libwayland-server.a: $(stat -c%s "$SYSROOT/usr/lib/libwayland-server.a") bytes"
}

install_headers() {
    log "installing headers + wayland.xml"
    mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/share/wayland"
    cp "$WL_SRC/src/wayland-client-core.h"  "$SYSROOT/usr/include/"
    cp "$WL_SRC/src/wayland-client.h"       "$SYSROOT/usr/include/"
    cp "$WL_SRC/src/wayland-server-core.h"  "$SYSROOT/usr/include/"
    cp "$WL_SRC/src/wayland-server.h"       "$SYSROOT/usr/include/"
    cp "$WL_SRC/src/wayland-util.h"         "$SYSROOT/usr/include/"
    cp "$WL_SRC/src/wayland-version.h"      "$SYSROOT/usr/include/"
    cp "$WL_SRC/generated/wayland-client-protocol.h" "$SYSROOT/usr/include/"
    cp "$WL_SRC/generated/wayland-server-protocol.h" "$SYSROOT/usr/include/"
    cp "$WL_SRC/protocol/wayland.xml"       "$SYSROOT/usr/share/wayland/"
    # wayland-cursor header
    cp "$WL_SRC/cursor/wayland-cursor.h"    "$SYSROOT/usr/include/"
}

# ── libwayland-cursor — cursor-theme loader used by sway + GTK clients
build_cursor_lib() {
    local objdir="$BUILD_DIR/wayland_cursor_objs"
    mkdir -p "$objdir"

    local cflags=(
        -O2 -fPIC -std=gnu11
        -Wall
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-missing-field-initializers
        -DHAVE_CONFIG_H
        -I "$WL_SRC"
        -I "$WL_SRC/src"
        -I "$WL_SRC/generated"
        -I "$WL_SRC/cursor"
        --sysroot="$SYSROOT"
        -nostdinc
        -isystem "$SYSROOT/usr/include"
    )

    local srcs=(
        cursor/wayland-cursor.c
        cursor/xcursor.c
        cursor/os-compatibility.c
    )

    local objs=()
    for src in "${srcs[@]}"; do
        local o="$objdir/$(basename "$src" .c).o"
        objs+=("$o")
        if [ "$WL_SRC/$src" -nt "$o" ]; then
            "$CROSS_CC" "${cflags[@]}" -c "$WL_SRC/$src" -o "$o"
        fi
    done

    rm -f "$SYSROOT/usr/lib/libwayland-cursor.a"
    "$CROSS_AR" rcs "$SYSROOT/usr/lib/libwayland-cursor.a" "${objs[@]}"
    log "libwayland-cursor.a: $(stat -c%s "$SYSROOT/usr/lib/libwayland-cursor.a") bytes"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/wayland_objs" \
               "$HOST_TOOLS/bin/wayland-scanner" \
               "$SYSROOT/usr/lib/libwayland-client.a" \
               "$SYSROOT/usr/lib/libwayland-server.a"
    fi
    fetch
    build_scanner
    generate_core_proto
    build_target_libs
    build_cursor_lib
    install_headers
    log "done"
}

main "$@"
