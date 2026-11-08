#!/usr/bin/env bash
# ── MakaOS wlroots port ────────────────────────────────────────────────
#
# Upstream-clean wlroots 0.18.1 build.  Input comes from libinput
# (ported separately); no custom backend shim.  Backends: drm +
# libinput; renderer is pixman-only (GL/Vulkan not ported yet);
# session via our native libseat+libudev.
#
# Outputs sysroot/usr/lib/libwlroots-0.18.{a,so.*} and headers.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLR_VERSION="0.18.1"
BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
WLR_SRC="$THIRD_PARTY/wlroots-${WLR_VERSION}"
WLR_TARBALL="$THIRD_PARTY/wlroots-${WLR_VERSION}.tar.gz"
WLR_BUILD="$BUILD_DIR/wlroots_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH

log() { printf '[port-wlroots] %s\n' "$*" >&2; }

# ── Source (re-)extraction ────────────────────────────────────────
# Earlier iterations patched a custom native/ backend into the tree.
# If that's still present, it's stale — wipe and re-extract so the
# build is upstream-clean.  Also wipes if the tarball exists but the
# dir doesn't (first run after a `rm -rf` clean).
fetch() {
    [ -f "$WLR_TARBALL" ] || {
        log "FATAL: tarball missing at $WLR_TARBALL — populate third_party/ first"
        exit 1
    }
    if [ -d "$WLR_SRC/backend/native" ]; then
        log "stale native/ backend detected — re-extracting source"
        rm -rf "$WLR_SRC"
    fi
    [ -d "$WLR_SRC" ] || {
        log "extracting wlroots-${WLR_VERSION}"
        tar -xzf "$WLR_TARBALL" -C "$THIRD_PARTY"
    }
}
fetch

# ── Sysroot scaffolding required before meson can configure wlroots.
# Each item here is idempotent; safe to re-run.
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
# Backends: drm (native KMS) + libinput (input via libinput port).
# Renderer: gles2 over the static Mesa (EGL+GLESv2) stack; allocator: gbm.
# The virgl GL stack reached first light (docs/VIRGL_BRINGUP.md phase 3c), so
# wlroots now composites on the GPU (GLES2 over GBM/dmabuf) instead of pixman
# software.  Session support uses our native libseat equivalent.
if [ ! -d "$WLR_BUILD" ]; then
    log "meson setup ${WLR_BUILD}"
    meson setup "$WLR_BUILD" "$WLR_SRC" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        -Dbackends=drm,libinput \
        -Drenderers=gles2 \
        -Dallocators=gbm \
        -Dsession=enabled \
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

# Rewrite wlroots-0.18.pc so a STATIC meson consumer (sway, dwl) links the whole
# GL stack correctly.  The generated .pc only Requires egl/gbm/glesv2 (their .pc
# lists -lEGL/-lGLESv2/-lgbm but NOT the DRI megadriver, glapi, or the C++
# runtime Mesa needs), and static link ordering wants the mutually-referencing
# archives in ONE --start-group.  So drop egl/gbm/glesv2 from Requires and put a
# self-contained group in Libs: wlroots + the megadriver .so + EGL/GLESv2/gbm/
# glapi/expat/z, then libstdc++/libsupc++.  This is what the manual tinywl link
# spells out; folding it into the .pc lets meson-built compositors just work.
log "patching wlroots-0.18.pc for the static Mesa GL group"
python3 - "$SYSROOT/usr/lib/pkgconfig/wlroots-0.18.pc" <<'PYEOF'
import sys
p = sys.argv[1]
out = []
for ln in open(p):
    ln = ln.rstrip('\n')
    if ln.startswith('Requires:'):
        reqs = [r.strip() for r in ln[len('Requires:'):].split(',')]
        reqs = [r for r in reqs if r and r.split()[0] not in ('egl', 'gbm', 'glesv2')]
        out.append('Requires: ' + ', '.join(reqs))
    elif ln.startswith('Libs:'):
        out.append('Libs: -L${libdir} -Wl,--allow-multiple-definition '
                   '-Wl,--start-group -lwlroots-0.18 '
                   '${libdir}/dri/virtio_gpu_dri.so '
                   '-lEGL -lGLESv2 -lgbm -lglapi -lexpat -lz '
                   '-Wl,--end-group -lstdc++ -lsupc++ -lm -lrt')
    else:
        out.append(ln)
open(p, 'w').write('\n'.join(out) + '\n')
print('patched', p)
PYEOF

log "done — check $SYSROOT/usr/lib/libwlroots*"
ls -la "$SYSROOT"/usr/lib/libwlroots* 2>/dev/null || true
