#!/usr/bin/env bash
# ── MakaOS libinput port ──────────────────────────────────────────────
#
# Consumes /dev/input/eventN nodes exposed by the kernel evdev layer
# (EVIOCG* ioctls match Linux byte-for-byte).  Provides pointer
# acceleration, tap-to-click, palm rejection, key repeat, scroll
# inertia, device quirks — all the things we'd otherwise rewrite and
# get subtly wrong.  Every wlroots/wayland compositor consumes
# libinput directly, so porting it lets sway / Hyprland / weston /
# river / labwc / dwl run unmodified against our kernel evdev nodes.
#
# Disabled: libwacom (tablet quirks DB — not needed until we ship
# a tablet), GUI tools, docs, tests.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LI_VERSION="1.26.2"
LI_URL="https://gitlab.freedesktop.org/libinput/libinput/-/archive/${LI_VERSION}/libinput-${LI_VERSION}.tar.gz"
# Upstream archive URL uses the bare tag (no project prefix).

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/libinput-${LI_VERSION}"
TARBALL="$THIRD_PARTY/libinput-${LI_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/libinput_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-libinput] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "$LI_URL"
    [ -d "$SRC_DIR" ] || tar -xzf "$TARBALL" -C "$THIRD_PARTY"
}

# libinput vendors wrapper headers at include/linux/*.h that dispatch
# to include/linux/{linux,freebsd}/*.h based on __linux__ / __FreeBSD__.
# Our compiler defines __makaos__, so both branches miss and the
# compile sees only forward declarations of struct input_absinfo etc.
# Fix: copy the bundled linux/ uapi files to makaos/ and add a
# __makaos__ branch to each wrapper.  Same approach used for libevdev.
patch_vendored_headers() {
    local inc="$SRC_DIR/include/linux"
    [ -d "$inc/linux" ] || { log "unexpected: no vendored linux/ dir"; return 0; }
    if [ ! -d "$inc/makaos" ]; then
        log "copying bundled linux uapi → makaos/"
        cp -r "$inc/linux" "$inc/makaos"
    fi
    # Patch each wrapper .h under include/linux/ (non-recursive — there
    # are only a couple: input.h, possibly others across libinput versions).
    for w in "$inc"/*.h; do
        [ -f "$w" ] || continue
        local base="$(basename "$w")"
        if grep -q '__makaos__' "$w"; then continue; fi
        if grep -q '__linux__' "$w"; then
            log "patching wrapper: $base"
            # Insert __makaos__ branch after the __linux__ one.
            sed -i 's|#elif __FreeBSD__|#elif defined(__makaos__)\n#include "makaos/'"$base"'"\n#elif __FreeBSD__|' "$w"
        fi
    done

    # libinput's meson.build hardcodes `shared_library('input', …)`
    # with SONAME versioning and a GNU --version-script.  Our entire
    # sysroot is static .a — replace the whole block with a
    # static_library call stripped of the shared-only arguments.
    local mb="$SRC_DIR/meson.build"
    if grep -q "^lib_libinput = shared_library" "$mb"; then
        log "patching meson.build: shared_library → static_library (strip version/link_args)"
        python3 - "$mb" << 'PYEOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
# Replace the multi-line shared_library(…) block for the input lib.
pat = re.compile(
    r"lib_libinput = shared_library\('input',\s*.*?\)\s*\n",
    re.DOTALL)
new = (
    "lib_libinput = static_library('input',\n"
    "\t\tsrc_libinput,\n"
    "\t\tinclude_directories : [include_directories('.'), includes_include],\n"
    "\t\tdependencies : deps_libinput,\n"
    "\t\tinstall : true\n"
    "\t\t)\n"
)
s2, n = pat.subn(new, s, count=1)
if n != 1:
    print("patch_vendored_headers: failed to rewrite shared_library block", file=sys.stderr)
    sys.exit(1)
open(p, 'w').write(s2)
PYEOF
    fi
}

# libinput's meson needs the quirks dir + its data; we point it at the
# source tree copy and install the data/* files alongside the lib.  It
# will happily build without any quirk files for a system that has no
# exotic hardware — libinput falls back to defaults.
configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 wiping build dir"
        rm -rf "$OUT_DIR"
    fi
    [ -d "$OUT_DIR" ] && return 0
    log "meson setup"
    meson setup "$OUT_DIR" "$SRC_DIR" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        -Dtests=false \
        -Dinstall-tests=false \
        -Ddocumentation=false \
        -Ddebug-gui=false \
        -Dlibwacom=false \
        2>&1 | tee "$BUILD_DIR/libinput_meson.log"
}

build_install() {
    # Build only the library archive.  The tool executables
    # (libinput-debug-events, libinput-record, libinput-analyze, …)
    # pull in libc surface we haven't wired up yet — uname, scanf
    # against stdin, and so on — and are not needed by compositors.
    # `ninja install` requires every default target, so skip it and
    # install libinput.a + headers + pkg-config manually.
    log "ninja build (libinput.a only)"
    ninja -C "$OUT_DIR" libinput.a 2>&1 | tee "$BUILD_DIR/libinput_ninja.log"

    log "installing libinput.a + headers + pkg-config"
    install -Dm644 "$OUT_DIR/libinput.a" "$SYSROOT/usr/lib/libinput.a"
    install -Dm644 "$SRC_DIR/src/libinput.h" "$SYSROOT/usr/include/libinput.h"
    install -Dm644 "$OUT_DIR/meson-private/libinput.pc" "$SYSROOT/usr/lib/pkgconfig/libinput.pc"
    # libinput installs its header one level deep in the normal flow;
    # also copy libinput-version.h and libinput-util.h if present.
    for h in libinput-version.h; do
        if [ -f "$OUT_DIR/src/$h" ]; then
            install -Dm644 "$OUT_DIR/src/$h" "$SYSROOT/usr/include/$h"
        fi
    done

    # Device quirks — libinput reads these from /usr/share/libinput at
    # startup to apply per-device tuning (tap defaults, scroll behavior,
    # palm rejection thresholds…).  Missing them is a runtime warning
    # ("Failed to load device quirks — will negatively affect device
    # behavior") rather than a fatal error, but wiring them up means
    # touchpads and trackpoints get correct defaults when encountered.
    log "installing device quirks"
    mkdir -p "$SYSROOT/usr/share/libinput"
    cp -f "$SRC_DIR/quirks/"*.quirks "$SYSROOT/usr/share/libinput/"
}

main() {
    fetch
    patch_vendored_headers
    configure
    build_install
    log "done"
    ls -la "$SYSROOT/usr/lib/libinput"* 2>/dev/null
}
main "$@"
