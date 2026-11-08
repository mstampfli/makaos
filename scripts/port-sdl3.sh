#!/usr/bin/env bash
# ── MakaOS SDL3 port ──────────────────────────────────────────────────
#
# SDL3 (libSDL3) is the cross-platform application framework every
# modern Wayland-native app we care about links against for windowing,
# input translation, timers, threads, atomics, audio, and — eventually —
# accelerated rendering.  First port using the CMake cross-toolchain.
#
# Build scope for first cut:
#   * Wayland video backend (only — no X11)
#   * Software renderer (no OpenGL/GLES/Vulkan — we have no Mesa yet)
#   * Threads + timers + file + filesystem + stdlib + power
#   * Audio: dummy driver only (no ALSA/Pulse/Jack)
#   * No joystick / haptic / hidapi / camera / sensor (no drivers)
#
# Produces:
#   build/sysroot/usr/lib/libSDL3.a
#   build/sysroot/usr/include/SDL3/*.h
#   build/sysroot/usr/lib/pkgconfig/sdl3.pc

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export REPO_ROOT

SDL_VERSION="3.2.0"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/SDL3-${SDL_VERSION}"
TARBALL="$THIRD_PARTY/SDL3-${SDL_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/sdl3_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
TOOLCHAIN_FILE="$REPO_ROOT/scripts/makaos-toolchain.cmake"

export PATH="$BUILD_DIR/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-sdl3] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading SDL3 ${SDL_VERSION}"
        curl -fsSL -o "$TARBALL" "$SDL_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
    }
}

patch_sdl() {
    # GL mode: SDL's CheckWayland requires the wayland-egl + egl pkg-configs to
    # build the Wayland GLES2/EGL client path.  Mesa now provides egl.pc +
    # glesv2.pc and the wayland port provides libwayland-egl + wayland-egl.pc,
    # so we KEEP those requirements (unlike the earlier software-only cut, which
    # stripped them).  Re-extract the pristine sdlchecks.cmake in case a prior
    # software-only build stripped it, so the check is authoritative.
    local rel="SDL3-${SDL_VERSION}/cmake/sdlchecks.cmake"
    if [ -f "$TARBALL" ]; then
        tar -xzf "$TARBALL" -C "$THIRD_PARTY" "$rel" 2>/dev/null || true
        log "restored pristine sdlchecks.cmake (Wayland GLES2/EGL path kept)"
    fi
}

# MakaOS: EGL/GLES are statically linked (Mesa) and MakaOS has NO dlopen (dlfcn
# is a NULL stub), so SDL must not try to dlopen libEGL.so/libGLESv2.so -- it
# has to bind the EGL entry points from the already-linked symbols.  SDL already
# has that path behind SDL_VIDEO_STATIC_ANGLE (SDL_egl.c: skip both dlopen blocks
# and make LOAD_FUNC bind `egl_data->NAME = NAME`).  Define it -- all core EGL
# funcs are in libEGL.a and extensions still resolve via eglGetProcAddress, so
# nothing ANGLE-specific is pulled in.  Idempotent (marker-guarded).
patch_sdl_static_egl() {
    local f="$SRC_DIR/src/video/SDL_egl.c"
    if grep -q 'MAKAOS_STATIC_EGL' "$f" 2>/dev/null; then
        log "SDL_egl.c already has the static-EGL define"; return 0
    fi
    log "enabling SDL static-EGL path (no dlopen; bind linked Mesa EGL symbols)"
    python3 - "$f" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
needle = '#include "SDL_internal.h"'
assert needle in s, "SDL_egl.c: SDL_internal.h include not found"
s = s.replace(needle,
    needle + '\n'
    '/* MAKAOS_STATIC_EGL: EGL/GLES statically linked (Mesa), MakaOS has no\n'
    '   dlopen -- bind EGL entry points from the linked symbols, skip dlopen. */\n'
    '#ifndef SDL_VIDEO_STATIC_ANGLE\n'
    '#define SDL_VIDEO_STATIC_ANGLE 1\n'
    '#endif\n', 1)
open(p, 'w').write(s)
PYEOF
}

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping build dir"
        rm -rf "$OUT_DIR"
    fi
    [ -f "$OUT_DIR/CMakeCache.txt" ] && return 0
    mkdir -p "$OUT_DIR"
    log "cmake configure (static + Wayland only, no GL/Vulkan/audio HW)"
    cmake \
        -S "$SRC_DIR" \
        -B "$OUT_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_TEST=OFF \
        -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF \
        -DSDL_INSTALL_TESTS=OFF \
        -DSDL_WAYLAND=ON \
        -DSDL_WAYLAND_SHARED=OFF \
        -DSDL_WAYLAND_LIBDECOR=OFF \
        -DSDL_X11=OFF \
        -DSDL_KMSDRM=OFF \
        -DSDL_KMSDRM_SHARED=OFF \
        -DSDL_OPENGL=ON \
        -DSDL_OPENGLES=ON \
        -DSDL_RENDER_D3D=OFF \
        -DSDL_VULKAN=OFF \
        -DSDL_METAL=OFF \
        -DSDL_DUMMYVIDEO=ON \
        -DSDL_OFFSCREEN=ON \
        -DSDL_AUDIO=ON \
        -DSDL_MAKAOSAUDIO=ON \
        -DSDL_DUMMYAUDIO=ON \
        -DSDL_DISKAUDIO=ON \
        -DSDL_ALSA=OFF \
        -DSDL_PULSEAUDIO=OFF \
        -DSDL_JACK=OFF \
        -DSDL_PIPEWIRE=OFF \
        -DSDL_SNDIO=OFF \
        -DSDL_JOYSTICK=OFF \
        -DSDL_HAPTIC=OFF \
        -DSDL_HIDAPI=OFF \
        -DSDL_SENSOR=OFF \
        -DSDL_POWER=ON \
        -DSDL_THREADS=ON \
        -DSDL_TIMERS=ON \
        -DSDL_FILE=ON \
        -DSDL_FILESYSTEM=ON \
        -DSDL_DLOPEN=OFF \
        -DSDL_CAMERA=OFF \
        -DSDL_GPU=OFF \
        -DSDL_DIALOG=OFF \
        -DSDL_LIBUDEV=OFF \
        -DSDL_LIBICONV=OFF \
        -DSDL_RPATH=OFF \
        2>&1 | tee "$BUILD_DIR/sdl3_cmake.log"
}

build_install() {
    log "cmake build"
    cmake --build "$OUT_DIR" -j"$(nproc)" 2>&1 | tee "$BUILD_DIR/sdl3_build.log"

    log "cmake install → $SYSROOT/usr"
    DESTDIR="$SYSROOT" cmake --install "$OUT_DIR" 2>&1 | tee "$BUILD_DIR/sdl3_install.log"

    local lib="$SYSROOT/usr/lib/libSDL3.a"
    if [ -f "$lib" ]; then
        log "done — $(stat -c%s "$lib") bytes → $lib"
    else
        log "FAIL: libSDL3.a not installed"
        exit 1
    fi
}

# MakaOS: SDL3 3.2.0's Wayland backend implements no CreateWindowFramebuffer, so
# SDL_GetWindowSurface() -- and thus the software SDL_Renderer -- only works
# through the generic texture fallback, which needs a GPU render driver.  With
# no GL/GLES/Vulkan there is then no software path at all and SDL_CreateRenderer
# fails ("Couldn't find matching render driver").  Install a native wl_shm
# software framebuffer and wire it into the Wayland video device so software
# rendering works with no GPU.  Idempotent (safe to re-run; markers guard it).
patch_sdl_framebuffer() {
    local wl="$SRC_DIR/src/video/wayland"
    log "installing wl_shm software framebuffer into the Wayland backend"
    cp "$REPO_ROOT/scripts/sdl3-wayland-framebuffer/SDL_waylandframebuffer.c" "$wl/"
    cp "$REPO_ROOT/scripts/sdl3-wayland-framebuffer/SDL_waylandframebuffer.h" "$wl/"
    python3 - "$wl/SDL_waylandvideo.c" "$wl/SDL_waylandwindow.h" <<'PYEOF'
import sys
video, window = sys.argv[1], sys.argv[2]

s = open(video).read()
if 'SDL_waylandframebuffer.h' not in s:
    s = s.replace('#include "SDL_waylandclipboard.h"\n',
                  '#include "SDL_waylandclipboard.h"\n#include "SDL_waylandframebuffer.h"\n', 1)
if 'Wayland_CreateWindowFramebuffer' not in s:
    s = s.replace(
        '    device->CreateSDLWindow = Wayland_CreateWindow;\n',
        '    device->CreateSDLWindow = Wayland_CreateWindow;\n'
        '    // MakaOS: native wl_shm software framebuffer (SDL_waylandframebuffer.c)\n'
        '    device->CreateWindowFramebuffer = Wayland_CreateWindowFramebuffer;\n'
        '    device->UpdateWindowFramebuffer = Wayland_UpdateWindowFramebuffer;\n'
        '    device->DestroyWindowFramebuffer = Wayland_DestroyWindowFramebuffer;\n', 1)
open(video, 'w').write(s)

w = open(window).read()
if 'shm_framebuffer' not in w:
    w = w.replace(
        'struct SDL_WindowData\n{\n'
        '    SDL_Window *sdlwindow;\n'
        '    SDL_VideoData *waylandData;\n'
        '    struct wl_surface *surface;\n',
        'struct Wayland_SHMBuffer;   // MakaOS: wl_shm software framebuffer\n\n'
        'struct SDL_WindowData\n{\n'
        '    SDL_Window *sdlwindow;\n'
        '    SDL_VideoData *waylandData;\n'
        '    struct wl_surface *surface;\n'
        '    struct Wayland_SHMBuffer *shm_framebuffer;   // MakaOS: non-NULL when software-rendered\n', 1)
    assert 'shm_framebuffer' in w, "SDL_WindowData head not matched -- SDL layout changed"
    open(window, 'w').write(w)
PYEOF
}

# MakaOS: SDL3 3.2.0 ships no audio backend that targets MakaOS -- the only
# portable ones built here are dummy (silent) and disk (to a file); OSS/ALSA/
# Pulse are all absent.  Install a native backend that drives /dev/dsp (the
# kernel Intel HDA node) and wire it into the build + the bootstrap probe list
# AHEAD of disk/dummy so SDL selects it automatically.  Idempotent (every edit
# is guarded by a marker).
patch_sdl_audio() {
    local dst="$SRC_DIR/src/audio/makaos"
    log "installing native /dev/dsp audio backend into the audio subsystem"
    mkdir -p "$dst"
    cp "$REPO_ROOT/scripts/sdl3-makaos-audio/SDL_makaosaudio.c" "$dst/"
    cp "$REPO_ROOT/scripts/sdl3-makaos-audio/SDL_makaosaudio.h" "$dst/"
    python3 - "$SRC_DIR" <<'PYEOF'
import sys, os
root = sys.argv[1]

def patch(rel, needle, find, repl):
    p = os.path.join(root, rel)
    s = open(p).read()
    if needle in s:
        return
    assert find in s, "%s: anchor not found -- SDL layout changed" % rel
    s = s.replace(find, repl, 1)
    assert needle in s, "%s: patch did not apply" % rel
    open(p, 'w').write(s)

# 1. bootstrap[] probe array -- MakaOS before disk/dummy so it wins auto-probe.
patch('src/audio/SDL_audio.c', 'MAKAOSAUDIO_bootstrap',
      '#ifdef SDL_AUDIO_DRIVER_DISK\n    &DISKAUDIO_bootstrap,',
      '#ifdef SDL_AUDIO_DRIVER_MAKAOS\n    &MAKAOSAUDIO_bootstrap,\n#endif\n'
      '#ifdef SDL_AUDIO_DRIVER_DISK\n    &DISKAUDIO_bootstrap,')

# 2. extern bootstrap declaration.
patch('src/audio/SDL_sysaudio.h', 'MAKAOSAUDIO_bootstrap',
      'extern AudioBootStrap DISKAUDIO_bootstrap;',
      'extern AudioBootStrap MAKAOSAUDIO_bootstrap;\n'
      'extern AudioBootStrap DISKAUDIO_bootstrap;')

# 3a. CMake option.
patch('CMakeLists.txt', 'SDL_MAKAOSAUDIO',
      'dep_option(SDL_DISKAUDIO           "Support the disk writer audio driver" ON "SDL_AUDIO" OFF)',
      'dep_option(SDL_MAKAOSAUDIO         "Support the MakaOS native (/dev/dsp) audio driver" ON "SDL_AUDIO" OFF)\n'
      'dep_option(SDL_DISKAUDIO           "Support the disk writer audio driver" ON "SDL_AUDIO" OFF)')

# 3b. CMake option -> config define + source glob.
patch('CMakeLists.txt', 'SDL_AUDIO_DRIVER_MAKAOS 1',
      '  if(SDL_DISKAUDIO)\n'
      '    set(SDL_AUDIO_DRIVER_DISK 1)\n'
      '    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/audio/disk/*.c")\n'
      '    set(HAVE_DISKAUDIO TRUE)\n'
      '    set(HAVE_SDL_AUDIO TRUE)\n'
      '  endif()',
      '  if(SDL_MAKAOSAUDIO)\n'
      '    set(SDL_AUDIO_DRIVER_MAKAOS 1)\n'
      '    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/audio/makaos/*.c")\n'
      '    set(HAVE_MAKAOSAUDIO TRUE)\n'
      '    set(HAVE_SDL_AUDIO TRUE)\n'
      '  endif()\n'
      '  if(SDL_DISKAUDIO)\n'
      '    set(SDL_AUDIO_DRIVER_DISK 1)\n'
      '    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/audio/disk/*.c")\n'
      '    set(HAVE_DISKAUDIO TRUE)\n'
      '    set(HAVE_SDL_AUDIO TRUE)\n'
      '  endif()')

# 4. config header substitution (#cmakedefine -> #define when the var is set).
patch('include/build_config/SDL_build_config.h.cmake', 'SDL_AUDIO_DRIVER_MAKAOS',
      '#cmakedefine SDL_AUDIO_DRIVER_DISK 1',
      '#cmakedefine SDL_AUDIO_DRIVER_MAKAOS 1\n'
      '#cmakedefine SDL_AUDIO_DRIVER_DISK 1')
PYEOF
}

main() {
    fetch
    patch_sdl
    patch_sdl_static_egl
    patch_sdl_framebuffer
    patch_sdl_audio
    configure
    build_install
}

main "$@"
