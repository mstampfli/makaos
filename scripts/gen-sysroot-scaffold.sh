#!/usr/bin/env bash
# ── gen-sysroot-scaffold.sh — sysroot link/include scaffolding ────────
#
# Idempotent pieces every meson/cmake port needs before it can even
# configure, factored out of port-wlroots.sh (which keeps its own copy
# for standalone runs):
#
#   1. Placeholder archives for -lm/-lrt/-lpthread/-ldl — those symbols
#      all live in libc.a on MakaOS; the empty archives exist purely so
#      `cc ... -lm` resolves.
#   2. Compiler builtin freestanding headers symlinked into the sysroot
#      (-nostdinc hides them otherwise).
#   3. Refreshed pkg-config descriptors for everything in the sysroot.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${SYSROOT:-$REPO_ROOT/build/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
GCC_VER="$(ls "$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/" 2>/dev/null | head -1)"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/${GCC_VER:-14.2.0}/include"

log() { printf '[sysroot-scaffold] %s\n' "$*" >&2; }

mkdir -p "$SYSROOT/usr/lib" "$SYSROOT/usr/include" "$SYSROOT/usr/bin"

for lib in m rt pthread dl; do
    a="$SYSROOT/usr/lib/lib${lib}.a"
    if [ ! -f "$a" ]; then
        stub=$(mktemp --suffix=.c)
        printf '// MakaOS: %s lives in libc.a; placeholder so -l%s resolves.\n' "$lib" "$lib" > "$stub"
        o="${stub%.c}.o"
        "$CROSS_CC" --sysroot="$SYSROOT" -nostdinc -isystem "$SYSROOT/usr/include" -c "$stub" -o "$o"
        "$CROSS_AR" rcs "$a" "$o"
        rm -f "$stub" "$o"
        log "wrote placeholder $a"
    fi
done

for h in float.h stddef.h stdarg.h stdbool.h stdatomic.h iso646.h limits.h stdalign.h stdnoreturn.h; do
    if [ -f "$TC_INCLUDE/$h" ] && [ ! -e "$SYSROOT/usr/include/$h" ]; then
        ln -sf "$TC_INCLUDE/$h" "$SYSROOT/usr/include/$h"
    fi
done

"$REPO_ROOT/scripts/gen-pkgconfig.sh" >/dev/null
log "done"

# Freestanding libstdc++ omits the hosted C-wrapper headers
# (<cassert>, <cstring>, ...) that harfbuzz and other C++ ports use.
# Install our shims (scripts/cxx-shims/) into the toolchain's C++
# include dir, which is already on g++'s default search path.
CXXINC="$REPO_ROOT/toolchain/x86_64-pc-makaos/include/c++/${GCC_VER:-14.2.0}"
if [ -d "$CXXINC" ]; then
    # Force-copy: some shims (cstdlib) intentionally REPLACE the
    # freestanding originals, which omit malloc/free.
    cp -f "$REPO_ROOT"/scripts/cxx-shims/* "$CXXINC/"
    echo "[sysroot-scaffold] C++ wrapper shims installed"
fi
