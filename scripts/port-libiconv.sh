#!/usr/bin/env bash
# ── MakaOS native libiconv stub ────────────────────────────────────────
#
# GLib hard-requires iconv for its UTF-8 ↔ other-encoding path.
# MakaOS is UTF-8-only; identity conversions between UTF-8 aliases
# (UTF-8 / UTF8 / utf-8 / …) work, everything else returns EINVAL.
#
# TODO(scalability-debt-ledger): when legacy-encoding support matters
# (e.g. rendering email from ISO-8859-1 headers), swap this for a real
# libiconv port.  Today UTF-8 is the whole system.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-iconv] %s\n' "$*" >&2; }

SRC_DIR="$REPO_ROOT/userland/compat/linux/libiconv"
mkdir -p "$SRC_DIR"



obj="$BUILD_DIR/iconv_objs/iconv.o"
mkdir -p "$(dirname "$obj")"
"$CROSS_CC" -O2 -fPIC -Wall \
    --sysroot="$SYSROOT" -nostdinc -I "$SYSROOT/usr/include" \
    -c "$SRC_DIR/iconv.c" -o "$obj"
rm -f "$SYSROOT/usr/lib/libiconv.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libiconv.a" "$obj"

cp "$SRC_DIR/iconv.h" "$SYSROOT/usr/include/iconv.h"

mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cat > "$SYSROOT/usr/lib/pkgconfig/iconv.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: iconv
Description: MakaOS native iconv stub (UTF-8 identity only)
Version: 1.17
Libs: -L\${libdir} -liconv
Cflags: -I\${includedir}
EOF

log "libiconv.a: $(stat -c%s "$SYSROOT/usr/lib/libiconv.a") bytes"
log "done"
