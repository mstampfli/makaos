#!/usr/bin/env bash
# ── MakaOS libudev-zero port ──────────────────────────────────────────
#
# libudev-zero is a minimal drop-in libudev replacement — ~300 LOC of
# C, no systemd dependency.  Used by libinput + wlroots to enumerate
# input + graphics devices.  For MakaOS we hardcode the device list
# to our virtio-gpu + evdev node; the "udev database" is static.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UDEV_VERSION="1.0.3"
UDEV_URL="https://github.com/illiliti/libudev-zero/archive/refs/tags/${UDEV_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
UDEV_SRC="$THIRD_PARTY/libudev-zero-${UDEV_VERSION}"
UDEV_TARBALL="$THIRD_PARTY/libudev-zero-${UDEV_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

CFLAGS=(
    -O2 -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -D_GNU_SOURCE
)

log() { printf '[port-udev-zero] %s\n' "$*" >&2; }

fetch_udev() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$UDEV_TARBALL" ]; then
        log "downloading libudev-zero ${UDEV_VERSION}"
        curl -fSL --retry 3 -o "$UDEV_TARBALL" "$UDEV_URL"
    fi
    if [ ! -d "$UDEV_SRC" ]; then
        log "extracting libudev-zero"
        tar -xzf "$UDEV_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing libudev.h into $SYSROOT/usr/include"
    # Upstream ships it as udev.h; consumers expect libudev.h.
    cp "$UDEV_SRC/udev.h" "$SYSROOT/usr/include/libudev.h"
}

build_lib() {
    local build_objs="$BUILD_DIR/udev_objs"
    mkdir -p "$build_objs"

    local srcs=(
        "udev.c"
        "udev_list.c"
        "udev_device.c"
        "udev_enumerate.c"
        "udev_monitor.c"
        "udev_queue.c"
        "udev_hwdb.c"
    )

    log "compiling libudev-zero with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$UDEV_SRC/$name"
        [ -f "$src" ] || { log "missing $name (skip)"; continue; }
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libudev.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/udev_objs" "$SYSROOT/usr/lib/libudev.a"
    fi
    fetch_udev
    install_headers
    build_lib
}

main "$@"
