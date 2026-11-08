#!/usr/bin/env bash
# ── MakaOS native libseat — always-grant stub ──────────────────────────
#
# MakaOS has no VTs, no logind, no session switching.  Every device
# open is granted immediately.  We ship just the public libseat.h API
# shape so wlroots + anything else expecting libseat links cleanly and
# gets "enabled, always active" semantics.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-libseat] %s\n' "$*" >&2; }

SRC_DIR="$REPO_ROOT/userland/compat/linux/libseat"
mkdir -p "$SRC_DIR"

# ── libseat.h — upstream-compatible API shape ──────────────────────

# ── libseat.c — always-grant stub ──────────────────────────────────

# ── Compile + archive ──────────────────────────────────────────────
objdir="$BUILD_DIR/libseat_objs"
mkdir -p "$objdir"
log "compiling libseat"
"$CROSS_CC" -O2 -fPIC -Wall \
    --sysroot="$SYSROOT" -nostdinc -isystem "$SYSROOT/usr/include" \
    -c "$SRC_DIR/libseat.c" -o "$objdir/libseat.o"
rm -f "$SYSROOT/usr/lib/libseat.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libseat.a" "$objdir/libseat.o"

# ── Install header + pkg-config ────────────────────────────────────
cp "$SRC_DIR/libseat.h" "$SYSROOT/usr/include/libseat.h"
mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cat > "$SYSROOT/usr/lib/pkgconfig/libseat.pc" << 'EOF'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include

Name: libseat
Description: MakaOS native libseat (always-grant stub)
Version: 0.9.1
Libs: -L${libdir} -lseat
Cflags: -I${includedir}
EOF

log "libseat.a: $(stat -c%s "$SYSROOT/usr/lib/libseat.a") bytes"
log "done"
