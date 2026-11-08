#!/usr/bin/env bash
# port-glfw.sh - cross-build GLFW (Wayland backend) to the MakaOS sysroot.
#
# GLFW is what LWJGL (and therefore Minecraft) loads for its window, GL context
# and input.  MakaOS already has the whole foundation GLFW-Wayland sits on:
# wayland-client/cursor/egl, xkbcommon, EGL/GLESv2 and Mesa/virgl GL (proven by
# the sway compositor + DarkPlaces).  So we build real GLFW's Wayland backend
# rather than shimming SDL.
#
# Default build is the STATIC lib (libglfw3.a) for a standalone smoke test.
# BUILD_SHARED=1 builds libglfw.so instead (for LWJGL to dlopen) -- see note.
#
# Prereqs (all present after a prior `bash build.sh`): the wayland/xkb/EGL ports
# in build/sysroot, host wayland-scanner, and scripts/makaos-toolchain.cmake.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export REPO_ROOT
BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
TOOLCHAIN_FILE="$REPO_ROOT/scripts/makaos-toolchain.cmake"
GLFW_VER="3.4"
SRC_DIR="$THIRD_PARTY/glfw-${GLFW_VER}"
BLD_DIR="$SRC_DIR/build-makaos"

log() { echo "[port-glfw] $*"; }

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

# ── fetch ──────────────────────────────────────────────────────────────────
if [ ! -d "$SRC_DIR" ]; then
    log "downloading GLFW ${GLFW_VER}"
    curl -fsSL -o "$THIRD_PARTY/glfw-${GLFW_VER}.zip" \
        "https://github.com/glfw/glfw/releases/download/${GLFW_VER}/glfw-${GLFW_VER}.zip"
    (cd "$THIRD_PARTY" && unzip -q "glfw-${GLFW_VER}.zip")
fi

# Patch GLFW's module loader (src/posix_module.c): GLFW loads its backend libs
# (wayland-client/cursor/egl, xkbcommon, EGL, GLES) via runtime dlopen, but MakaOS
# ships them as static .a. When the .so is not dlopen-able, fall back to resolving
# symbols from the main program via RTLD_DEFAULT (a non-NULL sentinel handle marks
# that). NOTE: this only helps for symbols actually present + exported in the
# loading binary's dynsym -- which today means the PIC libs (wayland/xkbcommon).
# Mesa's EGL/GLES are non-PIC, so fully satisfying GLFW's dlopen still needs the
# Mesa+wayland stack as real .so's (rebuild Mesa -fPIC) -- see the GLFW notes in
# the JVM-port memory / SCALABILITY_DEBT.
PM="$SRC_DIR/src/posix_module.c"
if ! grep -q "_glfw_makaos_static_module" "$PM"; then
    python3 - "$PM" <<'PY'
import sys
f=sys.argv[1]; s=open(f).read()
s=s.replace(
"void* _glfwPlatformLoadModule(const char* path)\n{\n    return dlopen(path, RTLD_LAZY | RTLD_LOCAL);\n}",
"static int _glfw_makaos_static_module;\n\n"
"void* _glfwPlatformLoadModule(const char* path)\n{\n"
"    void* m = dlopen(path, RTLD_LAZY | RTLD_LOCAL);\n"
"    if (m) return m;\n"
"    return &_glfw_makaos_static_module;  /* MakaOS: fall back to static syms */\n}")
s=s.replace(
"void _glfwPlatformFreeModule(void* module)\n{\n    dlclose(module);\n}",
"void _glfwPlatformFreeModule(void* module)\n{\n    if (module != &_glfw_makaos_static_module) dlclose(module);\n}")
s=s.replace(
"GLFWproc _glfwPlatformGetModuleSymbol(void* module, const char* name)\n{\n    return dlsym(module, name);\n}",
"GLFWproc _glfwPlatformGetModuleSymbol(void* module, const char* name)\n{\n"
"    if (module == &_glfw_makaos_static_module) return (GLFWproc) dlsym(RTLD_DEFAULT, name);\n"
"    return dlsym(module, name);\n}")
open(f,'w').write(s)
print("[port-glfw] patched posix_module.c (RTLD_DEFAULT static fallback)")
PY
fi

# MakaOS has no cursor theme installed, and GLFW's Wayland init treats a missing
# theme as fatal + later derefs the (NULL) theme when the pointer enters. Make the
# theme non-fatal and guard the NULL-theme derefs -- a game draws its own cursor.
WW="$SRC_DIR/src/wl_window.c"; WI="$SRC_DIR/src/wl_init.c"
if ! grep -q "MakaOS: no cursor theme" "$WW"; then
    python3 - "$WI" "$WW" <<'PY'
import sys
wi, ww = sys.argv[1], sys.argv[2]
s=open(wi).read()
s=s.replace(
'    if (!_glfw.wl.cursorTheme)\n    {\n        _glfwInputError(GLFW_PLATFORM_ERROR,\n                        "Wayland: Failed to load default cursor theme");\n        return GLFW_FALSE;\n    }',
'    if (!_glfw.wl.cursorTheme)\n    {\n        _glfwInputError(GLFW_PLATFORM_ERROR,\n                        "Wayland: no cursor theme found, continuing without one");\n    }')
open(wi,'w').write(s)
s=open(ww).read()
# _glfwSetCursorWayland default-cursor path
s=s.replace(
'        if (cursor)\n            setCursorImage(window, &cursor->wl);\n        else\n        {\n            struct wl_cursor* defaultCursor =',
'        if (cursor)\n            setCursorImage(window, &cursor->wl);\n        else if (!_glfw.wl.cursorTheme)\n        {\n            /* MakaOS: no cursor theme -> leave the compositor default cursor. */\n        }\n        else\n        {\n            struct wl_cursor* defaultCursor =')
# pointerHandleMotion themed path
s=s.replace(
'            struct wl_cursor* cursor = wl_cursor_theme_get_cursor(theme, cursorName);\n            if (!cursor)\n                return;',
'            if (!theme)\n                return;\n\n            struct wl_cursor* cursor = wl_cursor_theme_get_cursor(theme, cursorName);\n            if (!cursor)\n                return;')
# _glfwCreateStandardCursorWayland
s=s.replace(
'    cursor->wl.cursor = wl_cursor_theme_get_cursor(_glfw.wl.cursorTheme, name);\n\n    if (_glfw.wl.cursorThemeHiDPI)',
'    if (!_glfw.wl.cursorTheme)\n    {\n        _glfwInputError(GLFW_CURSOR_UNAVAILABLE,\n                        "Wayland: no cursor theme, standard cursor unavailable");\n        return GLFW_FALSE;\n    }\n\n    cursor->wl.cursor = wl_cursor_theme_get_cursor(_glfw.wl.cursorTheme, name);\n\n    if (_glfw.wl.cursorThemeHiDPI)')
open(ww,'w').write(s)
print("[port-glfw] patched wl_init.c/wl_window.c (NULL cursor theme guards)")
PY
fi

SHARED_ARG="OFF"
[ "${BUILD_SHARED:-0}" = "1" ] && SHARED_ARG="ON"

# ── configure ──────────────────────────────────────────────────────────────
# Wayland only (no X11), no libdecor (client-side decorations are irrelevant for
# a fullscreen game and libdecor is unported).  wayland-scanner is a host tool,
# so it must be found on the host PATH, not the sysroot (the toolchain sets
# FIND_ROOT_PATH_MODE_PROGRAM=NEVER, which already does that).
log "cmake configure (Wayland backend, shared=${SHARED_ARG})"
rm -rf "$BLD_DIR"
cmake \
    -S "$SRC_DIR" -B "$BLD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS="${SHARED_ARG}" \
    -DGLFW_BUILD_WAYLAND=ON \
    -DGLFW_BUILD_X11=OFF \
    -DGLFW_BUILD_EXAMPLES=OFF \
    -DGLFW_BUILD_TESTS=OFF \
    -DGLFW_BUILD_DOCS=OFF \
    -DGLFW_LIBRARY_TYPE=$([ "$SHARED_ARG" = "ON" ] && echo SHARED || echo STATIC)

# ── build + install ────────────────────────────────────────────────────────
log "cmake build"
cmake --build "$BLD_DIR" -j"$(nproc)"

log "install into sysroot"
DESTDIR="$SYSROOT" cmake --install "$BLD_DIR"

log "done:"
find "$SYSROOT" -name "libglfw*" -newer "$SRC_DIR/CMakeLists.txt" 2>/dev/null | sed "s|$SYSROOT|  \$SYSROOT|"
ls "$SYSROOT/usr/include/GLFW/glfw3.h" 2>/dev/null && echo "  headers installed"
