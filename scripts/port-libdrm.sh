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
    # Patch xf86drm.c's drmGetDeviceNameFromFd2 — on Linux it walks
    # /sys/dev/char/<maj>:<min> which we don't have.  Force a simple
    # "return /dev/dri/card0" for our single-GPU setup.
    python3 -c "
p='$DRM_SRC/xf86drm.c'
with open(p) as f: s = f.read()
marker = 'drm_public char *drmGetDeviceNameFromFd2(int fd)\n{'
if marker in s and 'MAKAOS_DRM_FIXED_DEVNAME' not in s:
    replacement = marker + '\n    /* MAKAOS_DRM_FIXED_DEVNAME: single-GPU system; no sysfs. */\n    (void)fd;\n    return strdup(\"/dev/dri/card0\");'
    s = s.replace(marker, replacement, 1)
    with open(p, 'w') as f: f.write(s)
    print('patched drmGetDeviceNameFromFd2')
"
    # Patch drmGetNodeTypeFromFd -- Linux stats the fd and derives the node
    # type from the DRM minor (0-63 primary, 64-127 control, 128+ render).
    # MakaOS DOES expose the minor via st_rdev (rdev=(226<<8)|minor), so derive
    # it the real way: renderD128 (minor 128) MUST report DRM_NODE_RENDER, or
    # Mesa's loader_is_device_render_capable rejects it and GL init falls back
    # to (unavailable) software.  card0 (minor 0) stays DRM_NODE_PRIMARY.
    python3 -c "
p='$DRM_SRC/xf86drm.c'
with open(p) as f: s = f.read()
marker = 'drm_public int drmGetNodeTypeFromFd(int fd)\n{'
if marker in s and 'MAKAOS_NODE_TYPE_FROM_MINOR' not in s:
    body = ('\n    /* MAKAOS_NODE_TYPE_FROM_MINOR: derive from the DRM minor. */\n'
            '    {\n'
            '        struct stat _sb;\n'
            '        if (fstat(fd, &_sb) == 0) {\n'
            '            unsigned _mn = minor(_sb.st_rdev);\n'
            '            if (_mn >= 128) return DRM_NODE_RENDER;\n'
            '            if (_mn >= 64)  return DRM_NODE_CONTROL;\n'
            '            return DRM_NODE_PRIMARY;\n'
            '        }\n'
            '        return DRM_NODE_PRIMARY;\n'
            '    }')
    replacement = marker + body
    s = s.replace(marker, replacement, 1)
    with open(p, 'w') as f: f.write(s)
    print('patched drmGetNodeTypeFromFd (from minor)')
"
    # Patch drmGetRenderDeviceNameFromFd — same sysfs walk we can't do.
    # We DO have a render node now (Phase 2: /dev/dri/renderD128), and it is
    # the node Mesa's virgl winsys opens for GL.  Single-GPU system, so return
    # it unconditionally (mirrors drmGetDeviceNameFromFd2 -> card0 above).
    python3 -c "
p='$DRM_SRC/xf86drm.c'
with open(p) as f: s = f.read()
marker = 'drm_public char *drmGetRenderDeviceNameFromFd(int fd)\n{'
if marker in s and 'MAKAOS_RENDER_NODE' not in s:
    replacement = marker + '\n    /* MAKAOS_RENDER_NODE: single-GPU system; no sysfs. */\n    (void)fd;\n    return strdup(\"/dev/dri/renderD128\");'
    s = s.replace(marker, replacement, 1)
    with open(p, 'w') as f: f.write(s)
    print('patched drmGetRenderDeviceNameFromFd -> renderD128')
"
    # Synthesise the DRM device list.  drmGetDevice2/drmGetDevices2/
    # drmGetDeviceFromDevId walk /sys/dev/char to enumerate DRM devices; MakaOS
    # has no sysfs, so they would return nothing and Mesa's _eglFindDevice /
    # render-node discovery (and therefore eglInitialize on the GBM platform)
    # would fail.  Emit a single synthetic virtio-gpu device (card0 primary +
    # renderD128 render node, virtio PCI identity 1af4:1050) allocated via
    # drmDeviceAlloc so drmFreeDevice frees it correctly.  Early-return from
    # each entry point; the original sysfs body stays as dead code.
    DRM_SRC="$DRM_SRC" python3 - <<'PYEOF'
import os
p = os.path.join(os.environ['DRM_SRC'], 'xf86drm.c')
with open(p) as f: s = f.read()
if 'MAKAOS_SYNTH_DEVICE' in s:
    raise SystemExit(0)

synth = r'''/* MAKAOS_SYNTH_DEVICE: MakaOS has no sysfs, so drmGetDevice2/drmGetDevices2
 * cannot enumerate via /sys/dev/char.  Synthesise the single virtio-gpu device
 * (card0 primary + renderD128 render node, virtio PCI identity) so Mesa's
 * EGLDevice / render-node discovery works.  Allocated through drmDeviceAlloc so
 * drmFreeDevice frees it correctly. */
static drmDevicePtr makaos_synth_virtio_device(void)
{
    char *addr;
    drmDevicePtr dev = drmDeviceAlloc(DRM_NODE_RENDER, "/dev/dri/renderD128",
                                      sizeof(drmPciBusInfo),
                                      sizeof(drmPciDeviceInfo), &addr);
    if (!dev)
        return NULL;
    dev->available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
    strcpy(dev->nodes[DRM_NODE_PRIMARY], "/dev/dri/card0");
    strcpy(dev->nodes[DRM_NODE_RENDER], "/dev/dri/renderD128");
    dev->bustype = DRM_BUS_PCI;
    dev->businfo.pci = (drmPciBusInfoPtr)addr;
    dev->businfo.pci->domain = 0;
    dev->businfo.pci->bus    = 0;
    dev->businfo.pci->dev    = 4;
    dev->businfo.pci->func   = 0;
    addr += sizeof(drmPciBusInfo);
    dev->deviceinfo.pci = (drmPciDeviceInfoPtr)addr;
    dev->deviceinfo.pci->vendor_id    = 0x1af4;   /* Red Hat / virtio */
    dev->deviceinfo.pci->device_id    = 0x1050;   /* virtio-gpu */
    dev->deviceinfo.pci->subvendor_id = 0x1af4;
    dev->deviceinfo.pci->subdevice_id = 0x1100;
    dev->deviceinfo.pci->revision_id  = 0;
    return dev;
}

'''

m_fromdevid = 'drm_public int drmGetDeviceFromDevId(dev_t find_rdev, uint32_t flags, drmDevicePtr *device)\n{'
b_fromdevid = ('\n    /* MAKAOS: no sysfs; single synthesised device. */\n'
               '    (void)find_rdev; (void)flags;\n'
               '    *device = makaos_synth_virtio_device();\n'
               '    return *device ? 0 : -ENOMEM;\n')

m_dev2 = 'drm_public int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)\n{'
b_dev2 = ('\n    /* MAKAOS: no sysfs; single synthesised device. */\n'
          '    (void)fd; (void)flags;\n'
          '    *device = makaos_synth_virtio_device();\n'
          '    return *device ? 0 : -ENOMEM;\n')

m_devs2 = ('drm_public int drmGetDevices2(uint32_t flags, drmDevicePtr devices[],\n'
           '                              int max_devices)\n{')
b_devs2 = ('\n    /* MAKAOS: no sysfs; report the single synthesised device. */\n'
           '    (void)flags;\n'
           '    if (devices == NULL)\n'
           '        return 1;\n'
           '    if (max_devices < 1)\n'
           '        return 0;\n'
           '    devices[0] = makaos_synth_virtio_device();\n'
           '    return devices[0] ? 1 : -ENOMEM;\n')

for m in (m_fromdevid, m_dev2, m_devs2):
    if m not in s:
        raise SystemExit('port-libdrm: device-synth marker not found: ' + m.split('(')[0])

# Insert the helper just before the first user (drmGetDeviceFromDevId) and the
# early-return into each of the three entry points.
s = s.replace(m_fromdevid, synth + m_fromdevid + b_fromdevid, 1)
s = s.replace(m_dev2,  m_dev2  + b_dev2,  1)
s = s.replace(m_devs2, m_devs2 + b_devs2, 1)

with open(p, 'w') as f: f.write(s)
print('patched drmGetDevice2/drmGetDevices2/drmGetDeviceFromDevId (synth device)')
PYEOF
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
/* Minimal format-modifier name tables.  xf86drm.c declares the struct types
 * itself (drmFormatModifierInfo, drmFormatModifierVendorInfo) right before this
 * file is #included.  These back drmGetFormatModifierName /
 * drmGetFormatModifierVendor, used ONLY for human-readable logging (e.g.
 * wlroots dumps the EGL dmabuf modifier list at DEBUG).
 *
 * A MATCHED entry is strdup'd, so a NULL name is a landmine: the old
 * `{ 0, 0 }` stub matched DRM_FORMAT_MOD_LINEAR (modifier 0) AND vendor NONE
 * (0), and strdup(NULL) SIGSEGV'd inside strlen the first time wlroots logged
 * the LINEAR modifier.  Give the two well-known NONE-vendor modifiers real
 * names; any other modifier/vendor misses the tables and the lookup returns
 * NULL, which callers already render as "<unknown>".
 */
static const struct drmFormatModifierInfo drm_format_modifier_table[] = {
    { DRM_FORMAT_MOD_LINEAR,  "LINEAR"  },
    { DRM_FORMAT_MOD_INVALID, "INVALID" },
};

static const struct drmFormatModifierVendorInfo drm_format_modifier_vendor_table[] = {
    { DRM_FORMAT_MOD_VENDOR_NONE, "NONE" },
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
