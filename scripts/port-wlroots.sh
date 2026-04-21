#!/usr/bin/env bash
# ── MakaOS wlroots port ────────────────────────────────────────────────
#
# First attempt: minimal config — no optional backends (no drm, no
# libinput, no x11), pixman-only renderer, session support via our
# native libseat+libudev, no xwayland, no gbm.
#
# This validates:
#   * meson cross-compile setup lands
#   * pkg-config sysroot resolution works for our libs
#   * wlroots core + wayland + headless + multi backends build
#   * generated-protocol dispatch links correctly
#
# Outputs sysroot/usr/lib/libwlroots-0.18.so.* (and static if meson can).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLR_VERSION="0.18.1"
BUILD_DIR="$REPO_ROOT/build"
WLR_SRC="$BUILD_DIR/third_party/wlroots-${WLR_VERSION}"
WLR_BUILD="$BUILD_DIR/wlroots_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH

log() { printf '[port-wlroots] %s\n' "$*" >&2; }

if [ ! -d "$WLR_SRC" ]; then
    log "FATAL: source tree missing at $WLR_SRC — extract the tarball first"
    exit 1
fi

# ── MakaOS native backend patch ───────────────────────────────────
# Copy our native backend source into the wlroots tree and patch
# backend/meson.build to include it.  Idempotent: re-copies every
# invocation so upstream tarball re-extract doesn't lose our patch.
NATIVE_SRC="$REPO_ROOT/userland/wlroots-backend-native"
if [ -d "$NATIVE_SRC" ]; then
    mkdir -p "$WLR_SRC/backend/native" "$WLR_SRC/include/wlr/backend"
    cp "$NATIVE_SRC/backend.c"               "$WLR_SRC/backend/native/"
    cp "$NATIVE_SRC/keyboard.c"              "$WLR_SRC/backend/native/"
    cp "$NATIVE_SRC/pointer.c"               "$WLR_SRC/backend/native/"
    cp "$NATIVE_SRC/native.h"                "$WLR_SRC/backend/native/"
    cp "$NATIVE_SRC/meson.build"             "$WLR_SRC/backend/native/"
    cp "$NATIVE_SRC/wlr_backend_native.h"    "$WLR_SRC/include/wlr/backend/native.h"

    # Patch backend/meson.build to include the native subdir — idempotent.
    if ! grep -q "^subdir('native')" "$WLR_SRC/backend/meson.build"; then
        printf "\nsubdir('native')\n" >> "$WLR_SRC/backend/meson.build"
    fi
    log "native backend source + header installed"
fi

# ── Sysroot scaffolding required before meson can configure wlroots.
#    Each item here is idempotent; safe to re-run.  Moving them into
#    port-wlroots.sh means a fresh sysroot self-bootstraps.
#
# 1) Link-time placeholder archives for -lm / -lrt / -lpthread / -ldl.
#    Math/time/pthread/dl symbols all live in libc.a on MakaOS; these
#    empty archives exist so `cc ... -lm` resolves.
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include"
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

# 2) Compiler builtin freestanding headers (float.h, stddef.h, stdarg.h,
#    …).  `-nostdinc` hides them; symlink into sysroot so wlroots and
#    other meson-built ports find them without special cross-file args.
for h in float.h stddef.h stdarg.h stdbool.h stdatomic.h iso646.h limits.h stdalign.h stdnoreturn.h; do
    if [ -f "$TC_INCLUDE/$h" ] && [ ! -e "$SYSROOT/usr/include/$h" ]; then
        ln -sf "$TC_INCLUDE/$h" "$SYSROOT/usr/include/$h"
    fi
done

# 3) wayland-scanner symlink so the wayland-scanner.pc variable
#    ${bindir}/wayland-scanner resolves after PKG_CONFIG_SYSROOT_DIR
#    prefixes it.  Host tool lives outside the sysroot; this symlink
#    lets pkg-config hand meson an executable path inside the sysroot.
if [ ! -e "$SYSROOT/usr/bin/wayland-scanner" ]; then
    ln -sf "$REPO_ROOT/build/host-tools/bin/wayland-scanner" \
           "$SYSROOT/usr/bin/wayland-scanner"
fi

# 4) Regenerate pkg-config descriptors for all sysroot libs so meson's
#    `dependency('foo')` calls resolve.  Cheap (microseconds), always safe.
"$REPO_ROOT/scripts/gen-pkgconfig.sh" >/dev/null

# ── pkg-config configured to see the sysroot .pc files AND
#    prepend the sysroot prefix to -L/-I paths the .pc files emit.
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

if [ "${FORCE:-0}" = "1" ]; then
    log "FORCE=1 — wiping prior build dir"
    rm -rf "$WLR_BUILD"
fi

# ── meson configure (only re-runs if $WLR_BUILD is fresh) ─────────
if [ ! -d "$WLR_BUILD" ]; then
    log "meson setup ${WLR_BUILD}"
    meson setup "$WLR_BUILD" "$WLR_SRC" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        -Dbackends= \
        -Drenderers= \
        -Dallocators= \
        -Dsession=disabled \
        -Dxwayland=disabled \
        -Dxcb-errors=disabled \
        -Dexamples=false \
        -Dlibliftoff=disabled \
        -Dcolor-management=disabled \
        2>&1 | tee "$BUILD_DIR/wlroots_meson.log"
fi

log "ninja build"
ninja -C "$WLR_BUILD" 2>&1 | tee "$BUILD_DIR/wlroots_ninja.log"

log "installing into $SYSROOT"
DESTDIR="$SYSROOT" ninja -C "$WLR_BUILD" install \
    2>&1 | tee "$BUILD_DIR/wlroots_install.log"

log "done — check $SYSROOT/usr/lib/libwlroots*"
ls -la "$SYSROOT"/usr/lib/libwlroots* 2>/dev/null || true
