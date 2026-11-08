#!/usr/bin/env bash
# ── MakaOS Mesa port (static virgl GL stack) ───────────────────────────
#
# Builds Mesa's gallium `virgl` driver + EGL + GBM + GLES2 for MakaOS,
# FULLY STATIC (MakaOS has no dynamic loader -- dlopen/dlsym are stubs).
# The virgl gallium driver drives our virtio-gpu 3D render node
# (/dev/dri/renderD128, Phase 2) through libdrm's virtgpu ioctls.
#
# Static-linking design (no dlopen anywhere):
#   * -Ddefault_library=static: every Mesa lib is a .a.
#   * The DRI megadriver target (targets/dri) is patched from
#     shared_library -> static_library so __driDriverGetExtensions_virtio_gpu
#     links into the app instead of a dlopen'd libgallium_dri.so.
#   * src/loader/loader.c:loader_open_driver is patched so the single
#     compiled-in "virtio_gpu" driver resolves to that statically-linked
#     entrypoint directly, skipping dlopen. GBM + EGL both funnel through
#     loader_open_driver, so this one shim covers both.
#   * GBM uses its builtin_backends[] table (static), not a dlopen'd backend.
#
# Requires (kernel side): renderD128's DRM_IOCTL_VERSION name == "virtio_gpu"
# so loader_get_driver_for_fd() selects virgl from the static descriptor table.
#
# Outputs sysroot/usr/lib/{libEGL,libgbm,libglapi,libGLESv2,libgallium*}.a
# + headers (EGL/, GLES2/, KHR/, gbm.h).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESA_VERSION="24.0.9"
BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
MESA_SRC="$THIRD_PARTY/mesa-${MESA_VERSION}"
MESA_TARBALL="$THIRD_PARTY/mesa-${MESA_VERSION}.tar.xz"
MESA_URL="https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz"
MESA_BUILD="$BUILD_DIR/mesa_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include"

log() { printf '[port-mesa] %s\n' "$*" >&2; }

# ── Source fetch/extract ──────────────────────────────────────────────
fetch() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$MESA_TARBALL" ]; then
        log "downloading mesa ${MESA_VERSION}"
        curl -fSL --retry 3 -o "$MESA_TARBALL" "$MESA_URL"
    fi
    if [ ! -d "$MESA_SRC" ]; then
        log "extracting mesa"
        tar -xJf "$MESA_TARBALL" -C "$THIRD_PARTY"
    fi
}

# ── Static-linking patches (idempotent, guarded) ──────────────────────
apply_patches() {
    # Patch 1: loader_open_driver resolves the single compiled-in virgl DRI
    # driver to its statically-linked entrypoint, no dlopen.
    python3 - "$MESA_SRC/src/loader/loader.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'MAKAOS_STATIC_DRI' not in s:
    anchor = ('loader_open_driver(const char *driver_name,\n'
              '                   void **out_driver_handle,\n'
              '                   const char **search_path_vars)\n{\n')
    shim = anchor + (
        '#ifdef MAKAOS_STATIC_DRI\n'
        '   /* MAKAOS_STATIC_DRI: the single virgl DRI driver is linked in\n'
        '    * statically; MakaOS has no dlopen. Resolve its entrypoint\n'
        '    * directly. */\n'
        '   extern const struct __DRIextensionRec **\n'
        '      __driDriverGetExtensions_virtio_gpu(void);\n'
        '   if (strcmp(driver_name, "virtio_gpu") == 0) {\n'
        '      *out_driver_handle = NULL;\n'
        '      return __driDriverGetExtensions_virtio_gpu();\n'
        '   }\n'
        '#endif\n')
    assert anchor in s, "loader_open_driver anchor not found"
    s = s.replace(anchor, shim, 1)
    open(p, 'w').write(s)
    print("patched loader_open_driver (static DRI shim)")

# loader_open_driver_lib only ever dlopens; on MakaOS return NULL so GBM's
# backend_from_driver_name falls through to its builtin backend (and it can't
# crash on a NULL default_search_path).
if 'MAKAOS_STATIC_DRI' not in s.split('loader_open_driver_lib')[1][:400]:
    anchor2 = ('{\n   char path[PATH_MAX];\n'
               '   const char *search_paths, *next, *end;\n\n'
               '   search_paths = NULL;\n')
    add2 = ('{\n   char path[PATH_MAX];\n'
            '   const char *search_paths, *next, *end;\n\n'
            '#ifdef MAKAOS_STATIC_DRI\n'
            '   (void)driver_name; (void)lib_suffix; (void)search_path_vars;\n'
            '   (void)default_search_path; (void)warn_on_fail;\n'
            '   return NULL;\n'
            '#endif\n\n'
            '   search_paths = NULL;\n')
    assert anchor2 in s, "loader_open_driver_lib anchor not found"
    s = s.replace(anchor2, add2, 1)
    open(p, 'w').write(s)
    print("patched loader_open_driver_lib (no dlopen)")

# loader_is_device_render_capable uses drmGetDevice2 (needs sysfs, absent on
# MakaOS); use the DRM node type (our drmGetNodeTypeFromFd derives it from the
# fd minor -- renderD128 => DRM_NODE_RENDER).
if 'MAKAOS_STATIC_DRI' not in s.split('loader_is_device_render_capable')[1][:200]:
    anchor3 = 'loader_is_device_render_capable(int fd)\n{\n   drmDevicePtr dev_ptr;'
    add3 = ('loader_is_device_render_capable(int fd)\n{\n'
            '#ifdef MAKAOS_STATIC_DRI\n'
            '   return drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER;\n'
            '#endif\n'
            '   drmDevicePtr dev_ptr;')
    assert anchor3 in s, "loader_is_device_render_capable anchor not found"
    s = s.replace(anchor3, add3, 1)
    open(p, 'w').write(s)
    print("patched loader_is_device_render_capable (node type)")
PY

    # Patch 2: the DRI megadriver target must be a STATIC lib on MakaOS so
    # __driDriverGetExtensions_virtio_gpu links into the consumer instead of
    # a dlopen'd .so.
    python3 - "$MESA_SRC/src/gallium/targets/dri/meson.build" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if 'MAKAOS_STATIC_DRI_TARGET' not in s:
    s = s.replace('libgallium_dri = shared_library(',
                  '# MAKAOS_STATIC_DRI_TARGET: static, no dlopen on MakaOS\n'
                  'libgallium_dri = static_library(', 1)
    open(p, 'w').write(s)
    print("patched dri target -> static_library")
PY

    # Patch 3: MakaOS IS a KMS/DRM platform (virtio-gpu KMS, dumb buffers,
    # /dev/dri, GBM-able). Add it to system_has_kms_drm so GBM + the drm
    # EGL platform enable. We keep host system == 'makaos' (honest) and
    # allowlist it where a gate is legitimately true for us, rather than
    # masquerading as 'linux' and pulling in Linux-isms we lack.
    python3 - "$MESA_SRC/meson.build" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
if "'makaos'" not in s:
    old = ("system_has_kms_drm = ['openbsd', 'netbsd', 'freebsd', "
           "'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android', "
           "'managarm'].contains(host_machine.system())")
    new = ("system_has_kms_drm = ['openbsd', 'netbsd', 'freebsd', "
           "'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android', "
           "'managarm', 'makaos'].contains(host_machine.system())")
    assert old in s, "system_has_kms_drm line not found"
    s = s.replace(old, new, 1)
    open(p, 'w').write(s)
    print("patched system_has_kms_drm += makaos")
PY

    # Patch 5: every library MakaOS builds must be STATIC -- libc.a is non-PIC
    # (static TLS uses R_X86_64_TPOFF32), so a shared object cannot link it.
    # These targets are explicit shared_library() (unaffected by
    # default_library=static), so convert them and drop the shared-only kwargs.
    python3 - "$MESA_SRC" <<'PY'
import sys, re, os
root = sys.argv[1]
targets = [
    "src/mapi/shared-glapi/meson.build", "src/egl/meson.build",
    "src/mapi/es2api/meson.build", "src/mapi/es1api/meson.build",
    "src/gbm/meson.build",
]
kw = [r"^\s*version\s*:\s*[a-zA-Z_][a-zA-Z0-9_]*\s*,?\s*$\n",
      r"^\s*version\s*:\s*'[0-9][^\n]*\n",
      r"^\s*soversion\s*:.*\n", r"^\s*darwin_versions\s*:.*\n",
      r"^\s*vs_module_defs\s*:.*\n"]
for rel in targets:
    p = os.path.join(root, rel); s = open(p).read()
    if "MAKAOS_STATIC_LIB" in s: continue
    o = s; s = s.replace("shared_library(", "static_library(")
    for pat in kw: s = re.sub(pat, "", s, flags=re.M)
    if s != o:
        open(p, 'w').write("# MAKAOS_STATIC_LIB: shared_library -> static (no PIC libc)\n" + s)
        print("static_library:", rel)
PY

    # Patch 6: virgl's shader disk cache uses build_id, which needs
    # dl_iterate_phdr (absent on static MakaOS). shader-cache is disabled anyway,
    # so run without a disk cache.
    python3 - "$MESA_SRC/src/gallium/drivers/virgl/virgl_screen.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if 'MAKAOS_NO_DISK_CACHE' not in s:
    start = ("   const struct build_id_note *note =\n"
             "      build_id_find_nhdr_for_addr(virgl_disk_cache_create);")
    end = '   screen->disk_cache = disk_cache_create("virgl", timestamp, 0);'
    i = s.find(start); j = s.find(end)
    assert i != -1 and j != -1, "virgl_disk_cache_create body not found"
    s = s[:i] + ("   /* MAKAOS_NO_DISK_CACHE: no dl_iterate_phdr for build_id and\n"
                 "    * shader-cache is disabled -- run without a disk cache. */\n"
                 "   screen->disk_cache = NULL;") + s[j + len(end):]
    open(p, 'w').write(s); print("stubbed virgl_disk_cache_create")
PY

}

# ── Sysroot scaffolding (same pattern as the other meson ports) ───────
scaffold() {
    mkdir -p "$SYSROOT/usr/lib" "$SYSROOT/usr/include" "$SYSROOT/usr/bin"
    # -lm/-lrt/-lpthread/-ldl placeholders (symbols live in libc.a).
    for lib in m rt pthread dl; do
        a="$SYSROOT/usr/lib/lib${lib}.a"
        if [ ! -f "$a" ]; then
            stub=$(mktemp --suffix=.c)
            printf '// MakaOS: %s in libc.a; placeholder so -l%s resolves.\n' "$lib" "$lib" > "$stub"
            o="${stub%.c}.o"
            "$CROSS_CC" --sysroot="$SYSROOT" -nostdinc -isystem "$SYSROOT/usr/include" -c "$stub" -o "$o"
            "$CROSS_AR" rcs "$a" "$o"; rm -f "$stub" "$o"
        fi
    done
    # freestanding compiler headers into the sysroot (-nostdinc hides them).
    for h in float.h stddef.h stdarg.h stdbool.h stdatomic.h iso646.h limits.h \
             stdalign.h stdnoreturn.h; do
        [ -f "$TC_INCLUDE/$h" ] && [ ! -e "$SYSROOT/usr/include/$h" ] && \
            ln -sf "$TC_INCLUDE/$h" "$SYSROOT/usr/include/$h" || true
    done
    # wayland-egl-backend: Mesa builds libwayland-egl itself and needs the
    # backend ABI header + a version>=3 pkg-config file. Upstream wayland ships
    # these, but our wayland port built the EGL stub and skipped them. Provide
    # them straight from the wayland source tree (v3 backend ABI in 1.23.x).
    local wl_src="$THIRD_PARTY/wayland-1.23.1"
    if [ -f "$wl_src/egl/wayland-egl-backend.h" ]; then
        cp "$wl_src/egl/wayland-egl-backend.h" "$SYSROOT/usr/include/"
        cat > "$SYSROOT/usr/lib/pkgconfig/wayland-egl-backend.pc" <<'PC'
prefix=/usr
includedir=${prefix}/include
Name: wayland-egl-backend
Description: Wayland EGL backend interface
Version: 3
Cflags: -I${includedir}
PC
    fi
    # Mesa's util/libsync.h is self-contained (sync_file structs inlined) but a
    # few TUs include it with <> instead of "". Put it on the sysroot path.
    if [ -f "$MESA_SRC/src/util/libsync.h" ]; then
        cp "$MESA_SRC/src/util/libsync.h" "$SYSROOT/usr/include/libsync.h"
    fi
    "$REPO_ROOT/scripts/gen-pkgconfig.sh" >/dev/null
}

fetch
apply_patches
scaffold

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

if [ "${FORCE:-0}" = "1" ]; then
    log "FORCE=1 — wiping prior build dir"
    rm -rf "$MESA_BUILD"
fi

# Build-wide defines (appended to the cross file's c_args by meson):
#   __linux__          MakaOS is Linux-syscall/ABI compatible (it ships the
#                      linux/ + asm/ shim headers); this takes the Linux branch
#                      in Mesa's raw `#if defined(__linux__)` gates (drm-uapi
#                      ioctl path, os detection, ...), same as the libdrm port.
#   MAKAOS_STATIC_DRI  enables the no-dlopen static DRI resolution shim.
# -Wno-error=format*: MakaOS's <stdint.h> types the u64 as `unsigned long long`
# while the compiler ABI (and thus code that hits the toolchain <stdint.h>) has
# it as `unsigned long`; PRIu64 stays "llu".  The two are the same WIDTH, so the
# format check is a benign type-identity gripe here -- keep it a warning, not a
# build-stopping error, rather than churning the whole libc's u64/PRIu64 ABI.
MESA_DEFS="-D__linux__ -DMAKAOS_STATIC_DRI=1 -Wno-error=format -Wno-error=format-security"
export CFLAGS="${CFLAGS:-} $MESA_DEFS"
export CXXFLAGS="${CXXFLAGS:-} $MESA_DEFS"

if [ ! -d "$MESA_BUILD" ]; then
    log "meson setup ${MESA_BUILD}"
    meson setup "$MESA_BUILD" "$MESA_SRC" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Dc_args="$MESA_DEFS" \
        -Dcpp_args="$MESA_DEFS" \
        -Db_staticpic=true \
        -Ddefault_library=static \
        -Dgallium-drivers=virgl \
        -Dvulkan-drivers= \
        -Dplatforms=wayland \
        -Degl=enabled \
        -Dgbm=enabled \
        -Dgles2=enabled \
        -Dopengl=true \
        -Dglx=disabled \
        -Dglvnd=false \
        -Dllvm=disabled \
        -Dshared-glapi=enabled \
        -Dgallium-vdpau=disabled \
        -Dgallium-va=disabled \
        -Dgallium-xa=disabled \
        -Dlmsensors=disabled \
        -Dvalgrind=disabled \
        -Dlibunwind=disabled \
        -Dbuild-tests=false \
        -Dzstd=disabled \
        -Dshader-cache=disabled \
        2>&1 | tee "$BUILD_DIR/mesa_meson.log"
fi

log "ninja build"
ninja -C "$MESA_BUILD" 2>&1 | tee "$BUILD_DIR/mesa_ninja.log"

log "installing into $SYSROOT"
DESTDIR="$SYSROOT" ninja -C "$MESA_BUILD" install 2>&1 | tee "$BUILD_DIR/mesa_install.log"

# ── Static-link symbol-collision fix ──────────────────────────────────────
# When the whole GL stack is statically linked into one binary (tinywl/sway/
# gltest with -Wl,--allow-multiple-definition), the EGL frontend and the DRI
# megadriver each define a GLOBAL dri2_lookup_egl_image_validated / *_egl_image
# etc. with DIFFERENT signatures (the EGL copy reads its 1st arg as the image,
# the gallium copy the 2nd).  --allow-multiple-definition collapses them to one
# and the EGL copy wins, so the DRI frontend calls it with swapped args and gets
# a garbage __DRIimage->texture -> #GP in dri_get_egl_image on the first frame.
# Normally these are hidden inside separate .so's; the monolithic static link
# exposes them.  Localize the EGL frontend's copies so each frontend uses its
# own (the megadriver's stay global for the gallium code).
if command -v objcopy >/dev/null 2>&1 && [ -f "$SYSROOT/usr/lib/libEGL.a" ]; then
    for s in dri2_lookup_egl_image_validated dri2_lookup_egl_image \
             dri2_validate_egl_image loader_dri_create_image \
             loader_image_format_to_fourcc; do
        objcopy --localize-symbol="$s" "$SYSROOT/usr/lib/libEGL.a" 2>/dev/null || true
    done
    log "localized EGL-frontend image symbols in libEGL.a (static-link collision fix)"
fi

log "done — check $SYSROOT/usr/lib/{libEGL,libgbm,libGLESv2}.a"
ls -la "$SYSROOT"/usr/lib/lib{EGL,gbm,GLESv2,glapi,gallium}* 2>/dev/null || true
