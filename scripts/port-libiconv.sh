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

SRC_DIR="$REPO_ROOT/userland/libiconv"
mkdir -p "$SRC_DIR"

cat > "$SRC_DIR/iconv.h" << 'EOF'
#ifndef _MAKAOS_ICONV_H
#define _MAKAOS_ICONV_H 1

#include <stddef.h>

typedef void* iconv_t;

iconv_t iconv_open(const char* tocode, const char* fromcode);
size_t  iconv(iconv_t cd, char** inbuf, size_t* inbytesleft,
                          char** outbuf, size_t* outbytesleft);
int     iconv_close(iconv_t cd);

#endif
EOF

cat > "$SRC_DIR/iconv.c" << 'EOF'
// MakaOS identity-only iconv.  UTF-8 is MakaOS's canonical encoding;
// every conversion between UTF-8 aliases (case-insensitive) is a byte
// copy.  Anything else returns EINVAL / (iconv_t)-1.

#include "iconv.h"
#include <errno.h>
#include <string.h>

static int is_utf8(const char* code) {
    if (!code) return 0;
    // Case-insensitive comparison to "UTF-8" / "UTF8" / "US-ASCII".
    // MakaOS treats US-ASCII as a subset of UTF-8 for transport.
    const char* aliases[] = { "UTF-8", "UTF8", "US-ASCII", "ASCII", 0 };
    for (int i = 0; aliases[i]; i++) {
        const char* a = aliases[i]; const char* b = code;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

iconv_t iconv_open(const char* to, const char* from) {
    if (is_utf8(to) && is_utf8(from)) return (iconv_t)(size_t)1;
    errno = EINVAL;
    return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inleft,
                         char** outbuf, size_t* outleft) {
    (void)cd;
    if (!inbuf || !*inbuf) return 0;   // state reset — no-op
    size_t n = *inleft < *outleft ? *inleft : *outleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf  += n; *inleft  -= n;
    *outbuf += n; *outleft -= n;
    if (*inleft > 0) { errno = E2BIG; return (size_t)-1; }
    return 0;
}

int iconv_close(iconv_t cd) { (void)cd; return 0; }
EOF

obj="$BUILD_DIR/iconv_objs/iconv.o"
mkdir -p "$(dirname "$obj")"
"$CROSS_CC" -O2 -fPIC -Wall \
    --sysroot="$SYSROOT" -nostdinc -I "$SYSROOT/usr/include" \
    -c "$SRC_DIR/iconv.c" -o "$obj"
rm -f "$SYSROOT/usr/lib/libiconv.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libiconv.a" "$obj"

cp "$SRC_DIR/iconv.h" "$SYSROOT/usr/include/iconv.h"

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
