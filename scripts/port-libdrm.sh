#!/usr/bin/env bash
# ── MakaOS libdrm port (cross-gcc + sysroot) ──────────────────────────
#
# libdrm is the userland wrapper over the DRM ioctls our kernel
# already implements on /dev/dri/card0.  Most of the library is
# per-driver extensions (radeon, intel, amdgpu, etc.) which we don't
# need — we only build the core (xf86drm + xf86drmMode) plus nothing.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRM_VERSION="2.4.125"
DRM_URL="https://dri.freedesktop.org/libdrm/libdrm-${DRM_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
DRM_SRC="$THIRD_PARTY/libdrm-${DRM_VERSION}"
DRM_TARBALL="$THIRD_PARTY/libdrm-${DRM_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

CFLAGS=(
    -O2 -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-pointer-arith
    -DHAVE_CONFIG_H
    -DHAVE_VISIBILITY=1
    -D_GNU_SOURCE
    -D__linux__        # libdrm's drm.h splits Linux vs BSD paths by
                       # this macro; MakaOS's DRM ioctl surface is
                       # Linux-compatible, so take the Linux branch.
                       # Our shim <linux/types.h> + <asm/ioctl.h>
                       # headers make that branch resolvable.
    -include sys/sysmacros.h   # major/minor/makedev
    -include stdio.h           # open_memstream
)

log() { printf '[port-libdrm] %s\n' "$*" >&2; }

fetch_drm() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$DRM_TARBALL" ]; then
        log "downloading libdrm ${DRM_VERSION}"
        curl -fSL --retry 3 -o "$DRM_TARBALL" "$DRM_URL"
    fi
    if [ ! -d "$DRM_SRC" ]; then
        log "extracting libdrm"
        tar -xJf "$DRM_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/libdrm"
    mkdir -p "$SYSROOT/usr/include/libdrm"
    cp "$DRM_SRC/xf86drm.h"           "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/xf86drmMode.h"       "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/include/drm/drm.h"           "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/include/drm/drm_mode.h"      "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/include/drm/drm_fourcc.h"    "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/include/drm/drm_sarea.h"     "$SYSROOT/usr/include/libdrm/"
    cp "$DRM_SRC/include/drm/i915_drm.h"      "$SYSROOT/usr/include/libdrm/" 2>/dev/null || true
    # MakaOS uses the Linux-style drm ABI (linux/types.h, asm/ioctl.h).
    # Add __makaos__ to the existing __linux__ guards so our cross-gcc
    # (which doesn't define __linux__) takes the Linux branch instead
    # of the BSD <sys/ioccom.h> path.
    for h in drm.h xf86drm.h; do
        python3 -c "
p='$SYSROOT/usr/include/libdrm/$h'
with open(p) as f: s = f.read()
s = s.replace('#if   defined(__linux__)', '#if   defined(__linux__) || defined(__makaos__)', 1)
s = s.replace('#if defined(__linux__)',   '#if defined(__linux__) || defined(__makaos__)', 1)
with open(p, 'w') as f: f.write(s)
"
    done
}

build_lib() {
    local build_objs="$BUILD_DIR/libdrm_objs"
    mkdir -p "$build_objs"

    # generated_static_table_fourcc.h is normally produced by a meson
    # script from fourcc_mapping.csv.  Provide a minimum-viable stub:
    # empty tables that drmGetFormatName / drmGetFormatModifierName
    # walk to return NULL on any lookup.  FOURCC constants themselves
    # come from drm_fourcc.h, not this generated file.  Reuses the
    # drmFormatModifierInfo / drmFormatModifierVendorInfo struct defs
    # that xf86drm.c declares just above the #include site.
    cat > "$DRM_SRC/generated_static_table_fourcc.h" <<'TABLESTUB'
/* Empty format-modifier lookup tables.  xf86drm.c declares the
 * struct types itself (drmFormatModifierInfo, drmFormatModifierVendorInfo)
 * right before this file is #included.  We provide zero-length tables
 * that drmGetFormatModifierName / drmGetFormatModifierVendor will
 * walk and always miss on, returning NULL — clients fall back to
 * returning "Unknown" or similar.  Populate with real entries later
 * if a port actually needs human-readable modifier names.
 */
static const struct drmFormatModifierInfo drm_format_modifier_table[] = {
    { 0, 0 }
};

static const struct drmFormatModifierVendorInfo drm_format_modifier_vendor_table[] = {
    { 0, 0 }
};
TABLESTUB

    # Hand-write a minimal config.h for libdrm.  It's a tiny file.
    cat > "$DRM_SRC/config.h" <<'EOF'
#pragma once
#define HAVE_SYS_MKDEV_H      0
#define HAVE_SYS_SYSCTL_H     0
#define HAVE_VISIBILITY       1
#define HAVE_OPEN_MEMSTREAM   0
#define MAJOR_IN_MKDEV        0
#define MAJOR_IN_SYSMACROS    1
#define PACKAGE               "libdrm"
#define PACKAGE_VERSION       "2.4.125"
#define VERSION               "2.4.125"
EOF

    local includes=(
        -I "$DRM_SRC"
        -I "$DRM_SRC/include/drm"
    )

    # Core libdrm only (xf86drm + xf86drmMode).  Skip all the per-
    # driver subdirs (amdgpu/, intel/, radeon/, freedreno/, etc.) —
    # virtio-gpu doesn't need any of them.
    local srcs=(
        "xf86drm.c"
        "xf86drmMode.c"
        "xf86drmHash.c"
        "xf86drmRandom.c"
        "xf86drmSL.c"
        "xf86atomic.h"    # header-only; listed so mtime check works
    )

    log "compiling libdrm sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        [ "${name##*.}" = "h" ] && continue
        local src="$DRM_SRC/$name"
        [ -f "$src" ] || { log "missing $name (skip)"; continue; }
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libdrm.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/libdrm_objs" "$SYSROOT/usr/lib/libdrm.a"
    fi
    fetch_drm
    install_headers
    build_lib
}

main "$@"
