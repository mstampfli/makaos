#!/usr/bin/env bash
# ── MakaOS mtdev stub ────────────────────────────────────────────────
#
# mtdev converts legacy multi-touch protocol A (one repeated set of
# ABS_MT_* axes per finger, no slots) into the modern protocol B
# (ABS_MT_SLOT + per-slot tracking IDs).  Our kernel emits protocol B
# natively on touch-capable devices (see evdev.h ABS_MT_SLOT), so
# mtdev just passes events through untouched.  libinput's dependency
# on mtdev is version-gated but otherwise trivial — it uses
# mtdev_new_open() / mtdev_get() / mtdev_delete().  A ~80 LOC stub
# suffices; if we ever grow a protocol-A capable touchscreen driver,
# upstream mtdev's 500 LOC A→B converter can drop in here.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-mtdev] %s\n' "$*" >&2; }

SRC="$REPO_ROOT/userland/compat/linux/mtdev"
mkdir -p "$SRC"



# Build static archive.
OBJ="$BUILD_DIR/mtdev_objs"
mkdir -p "$OBJ"
"$CROSS_CC" --sysroot="$SYSROOT" -nostdinc \
    -isystem "$SYSROOT/usr/include" \
    -isystem "$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include" \
    -I "$SRC" \
    -O2 -fPIC -Wall -c "$SRC/mtdev.c" -o "$OBJ/mtdev.o"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libmtdev.a" "$OBJ/mtdev.o"

# mtdev-plumbing.h is the internal-only header libinput reaches into.
# The only symbol it exports we actually need is mtdev_is_absmt_ranged(),
# a predicate upstream uses to decide MT protocol translation.  Our
# stub always returns 0 (we emit protocol B, no ranging needed).

# Install headers + pkg-config.
mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib/pkgconfig"
cp "$SRC/mtdev.h"          "$SYSROOT/usr/include/mtdev.h"
cp "$SRC/mtdev-plumbing.h" "$SYSROOT/usr/include/mtdev-plumbing.h"
cat > "$SYSROOT/usr/lib/pkgconfig/mtdev.pc" << EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: mtdev
Description: MakaOS pass-through mtdev (protocol A→B no-op)
Version: 1.1.6
Libs: -L\${libdir} -lmtdev
Cflags: -I\${includedir}
EOF

log "installed $(ls -la $SYSROOT/usr/lib/libmtdev.a)"
