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
    local f="$SRC_DIR/cmake/sdlchecks.cmake"
    if grep -q 'MAKAOS_PATCHED' "$f" 2>/dev/null; then
        log "already patched"; return 0
    fi
    # CheckWayland unconditionally requires wayland-egl + egl pkg-configs,
    # which only exist when Mesa is present.  We're doing a software-only
    # first cut (no GL/GLES/Vulkan), so drop those two modules from the
    # required spec and remove the matching FindLibraryAndSONAME.  The
    # wayland source fallback path is guarded by SDL_VIDEO_OPENGL_EGL,
    # which is 0 with SDL_OPENGL=OFF + SDL_OPENGLES=OFF.
    log "patching CheckWayland: drop wayland-egl + egl requirements"
    python3 - "$f" <<'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
# 1. Trim the pkg-config spec from 5 modules to 3.
s2 = s.replace(
    '"wayland-client>=1.18" wayland-egl wayland-cursor egl "xkbcommon>=0.5.0"',
    '"wayland-client>=1.18" wayland-cursor "xkbcommon>=0.5.0"',
    1)
assert s2 != s, "WAYLAND_PKG_CONFIG_SPEC line not found"
# 2. Drop the wayland-egl FindLibraryAndSONAME (only used by the shared
#    loader path which we don't build).
s3 = re.sub(
    r'\s*FindLibraryAndSONAME\(wayland-egl LIBDIRS[^)]*\)',
    '',
    s2, count=1)
assert s3 != s2, "FindLibraryAndSONAME(wayland-egl ...) not found"
# 3. Tag the file so re-runs skip the patch.
s3 = "# MAKAOS_PATCHED — scripts/port-sdl3.sh drops wayland-egl+egl reqs\n" + s3
open(p, 'w').write(s3)
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
        -DSDL_OPENGL=OFF \
        -DSDL_OPENGLES=OFF \
        -DSDL_RENDER_D3D=OFF \
        -DSDL_VULKAN=OFF \
        -DSDL_METAL=OFF \
        -DSDL_DUMMYVIDEO=ON \
        -DSDL_OFFSCREEN=ON \
        -DSDL_AUDIO=ON \
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

main() {
    fetch
    patch_sdl
    configure
    build_install
}

main "$@"
