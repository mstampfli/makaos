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
# wlroots' gles2 renderer pulls in the static Mesa stack, which is C++, so the
# final link must go through g++ to bring in libstdc++/libsupc++ and run the
# C++ static constructors (.init_array) via the default startup files.
CROSS_CXX="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-g++"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include"
# Prefer the host scanner built by port-wayland.sh; fall back to a system
# wayland-scanner (its protocol codegen is host-independent) so tinywl does not
# hard-fail when build/host-tools was not populated this run.
SCANNER="$REPO_ROOT/build/host-tools/bin/wayland-scanner"
[ -x "$SCANNER" ] || SCANNER="$(command -v wayland-scanner 2>/dev/null || true)"

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
"$CROSS_CC" "${cflags[@]}" -c "$WLR_SRC/tinywl/tinywl.c" -o "$BUILD_DIR/tinywl.o"

# Link through g++ (Mesa is C++).  Use the default startup (crt0 + crtbegin/
# crtend) rather than -nostartfiles so C++ static ctors in Mesa/wlroots run.
# The Mesa GL stack fragment mirrors the proven gltest link (build.sh): the
# megadriver .so + libEGL/GLESv2/gbm/glapi + expat, --gc-sections to drop the
# ~80 MB of unused Mesa, --allow-multiple-definition for the math symbols Mesa
# ships that MakaOS libc also provides.  wlroots + its own deps join the same
# --start-group since they cross-reference the GL libs (gles2 renderer).
GL_L="$SYSROOT/usr/lib"
log "linking tinywl against wlroots + the static Mesa GL stack (g++)"
"$CROSS_CXX" --sysroot="$SYSROOT" -m64 -mno-red-zone -no-pie -s \
    -Wl,--build-id=none \
    "$BUILD_DIR/tinywl.o" \
    -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--start-group \
    -lwlroots-0.18 \
    "$GL_L/dri/virtio_gpu_dri.so" \
    "$GL_L/libEGL.a" "$GL_L/libGLESv2.a" "$GL_L/libgbm.a" "$GL_L/libglapi.a" \
    "$GL_L/libexpat.a" \
    -lwayland-server -lwayland-client -lxkbcommon -lpixman-1 -ldrm -ludev \
    -lseat -lffi -ldisplay-info -linput -levdev -lmtdev \
    -Wl,--end-group \
    "$GL_L/libz.a" -lm -lrt -lpthread -ldl \
    -o "$BUILD_DIR/tinywl.elf"

mkdir -p "$SYSROOT/usr/bin"
cp "$BUILD_DIR/tinywl.elf" "$SYSROOT/usr/bin/tinywl"
log "tinywl.elf: $(stat -c%s "$BUILD_DIR/tinywl.elf") bytes → $SYSROOT/usr/bin/tinywl"
