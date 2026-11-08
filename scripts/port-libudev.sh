#!/usr/bin/env bash
# ── MakaOS native libudev equivalent ───────────────────────────────────
#
# Implements the subset of libudev that wlroots + our own stack need.
# NOT a port — written natively against MakaOS /dev/* contents.
# Static device list today (card0, event0); scales later (ledger #2).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-libudev] %s\n' "$*" >&2; }

SRC_DIR="$REPO_ROOT/userland/compat/linux/libudev"
mkdir -p "$SRC_DIR"

# ── libudev.h — API surface wlroots consumes ──────────────────────

# ── libudev.c — native implementation against /dev/* ───────────────

# ── Compile + archive ──────────────────────────────────────────────
objdir="$BUILD_DIR/libudev_objs"
mkdir -p "$objdir"
log "compiling libudev"
"$CROSS_CC" -O2 -fPIC -Wall -Wno-unused-parameter \
    --sysroot="$SYSROOT" -nostdinc -isystem "$SYSROOT/usr/include" \
    -c "$SRC_DIR/libudev.c" -o "$objdir/libudev.o"
rm -f "$SYSROOT/usr/lib/libudev.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libudev.a" "$objdir/libudev.o"

cp "$SRC_DIR/libudev.h" "$SYSROOT/usr/include/libudev.h"
mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cat > "$SYSROOT/usr/lib/pkgconfig/libudev.pc" << 'EOF'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include

Name: libudev
Description: MakaOS native libudev equivalent
Version: 255
Libs: -L${libdir} -ludev
Cflags: -I${includedir}
EOF

log "libudev.a: $(stat -c%s "$SYSROOT/usr/lib/libudev.a") bytes"
log "done"
