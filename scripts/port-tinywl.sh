#!/usr/bin/env bash
# ── MakaOS tinywl build ────────────────────────────────────────────────
#
# tinywl is wlroots' own ~1000-LOC reference compositor.  Pure wlroots +
# wayland-server + xkbcommon — zero libinput/cairo/pango/glib.  Exists
# in the wlroots source tree at tinywl/tinywl.c.
#
# Goal: produce $SYSROOT/usr/bin/tinywl that smoke-links against our
# sysroot.  Runtime validation is Tier 7 (inside QEMU with our native
# input backend active).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLR_SRC="$REPO_ROOT/build/third_party/wlroots-0.18.1"
BUILD_DIR="$REPO_ROOT/build/tinywl_build"
SYSROOT="${SYSROOT:-$REPO_ROOT/build/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include"
SCANNER="$REPO_ROOT/build/host-tools/bin/wayland-scanner"

log() { printf '[tinywl] %s\n' "$*" >&2; }

mkdir -p "$BUILD_DIR"

# Generate xdg-shell-protocol.h (tinywl includes this).
if [ ! -f "$BUILD_DIR/xdg-shell-protocol.h" ]; then
    log "generating xdg-shell-protocol.h"
    "$SCANNER" server-header \
        "$SYSROOT/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml" \
        "$BUILD_DIR/xdg-shell-protocol.h"
fi

cflags=(
    -O2 -std=gnu11
    -Wall -Wextra
    -Wno-unused-parameter
    -DWLR_USE_UNSTABLE
    --sysroot="$SYSROOT"
    -nostdinc
    -isystem "$SYSROOT/usr/include"
    -isystem "$TC_INCLUDE"
    -I "$BUILD_DIR"
    -I "$SYSROOT/usr/include/wlroots-0.18"
    -I "$SYSROOT/usr/include/pixman-1"
    -I "$SYSROOT/usr/include/libdrm"
)

log "cross-compiling tinywl.c"
"$CROSS_CC" "${cflags[@]}" \
    -nostartfiles -Wl,--build-id=none \
    "$SYSROOT/usr/lib/crt0.o" \
    "$WLR_SRC/tinywl/tinywl.c" \
    -Wl,--start-group \
    -lwlroots-0.18 -lwayland-server -lwayland-client \
    -lxkbcommon -lpixman-1 -ldrm -ludev -lseat -lffi \
    -lc -lm -lrt -lpthread -ldl \
    -Wl,--end-group \
    -o "$BUILD_DIR/tinywl.elf"

mkdir -p "$SYSROOT/usr/bin"
cp "$BUILD_DIR/tinywl.elf" "$SYSROOT/usr/bin/tinywl"
log "tinywl.elf: $(stat -c%s "$BUILD_DIR/tinywl.elf") bytes → $SYSROOT/usr/bin/tinywl"
