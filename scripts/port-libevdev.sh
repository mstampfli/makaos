#!/usr/bin/env bash
# ── MakaOS libevdev port (upstream) ───────────────────────────────────
#
# libinput depends on libevdev for struct libevdev device handling,
# event parsing via EVIOCG* ioctls, and slot tracking.  Prior to this
# commit we shipped a 2-function stub that only satisfied sway's
# libevdev_event_code_from_name() path; libinput exercises the full
# surface (libevdev_new_from_fd, libevdev_next_event, libevdev_has_
# event_code, slot management).  Ship upstream 1.13.1 cross-compiled
# against our sysroot — the kernel's EVIOCG* ioctls now match what
# libevdev expects byte-for-byte.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EVDEV_VERSION="1.13.1"
# Upstream tags are prefixed with the project name.
EVDEV_URL="https://gitlab.freedesktop.org/libevdev/libevdev/-/archive/libevdev-${EVDEV_VERSION}/libevdev-libevdev-${EVDEV_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/libevdev-libevdev-${EVDEV_VERSION}"
TARBALL="$THIRD_PARTY/libevdev-libevdev-${EVDEV_VERSION}.tar.gz"
OUT_DIR="$BUILD_DIR/libevdev_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PATH
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-libevdev] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || curl -fsSL -o "$TARBALL" "$EVDEV_URL"
    [ -d "$SRC_DIR" ] || tar -xzf "$TARBALL" -C "$THIRD_PARTY"
}

configure() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 wiping build dir"
        rm -rf "$OUT_DIR"
    fi
    [ -d "$OUT_DIR" ] && return 0
    # libevdev's meson looks up input.h / input-event-codes.h under
    # include/linux/<host_machine.system()>/.  Our cross-file sets
    # system=makaos; point that at the bundled linux copies (same
    # codes, Linux is the ABI we implement byte-for-byte).
    mkdir -p "$SRC_DIR/include/linux/makaos"
    cp "$SRC_DIR/include/linux/linux/input.h" "$SRC_DIR/include/linux/makaos/input.h"
    cp "$SRC_DIR/include/linux/linux/input-event-codes.h" "$SRC_DIR/include/linux/makaos/input-event-codes.h"
    cp "$SRC_DIR/include/linux/linux/uinput.h" "$SRC_DIR/include/linux/makaos/uinput.h"
    # Wrapper picks bundled kernel header via __linux__ / __FreeBSD__
    # macros.  Our cross-gcc defines neither; teach it __makaos__.
    for h in input.h input-event-codes.h uinput.h; do
        w="$SRC_DIR/include/linux/$h"
        if [ -f "$w" ] && ! grep -q "__makaos__" "$w"; then
            sed -i 's|#elif __FreeBSD__|#elif defined(__makaos__)\n#include "makaos/'"$h"'"\n#elif __FreeBSD__|' "$w"
        fi
    done
    log "meson setup"
    meson setup "$OUT_DIR" "$SRC_DIR" \
        --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
        --prefix=/usr \
        --libdir=lib \
        -Ddefault_library=static \
        -Dtests=disabled \
        -Ddocumentation=disabled \
        -Dcoverity=false \
        2>&1 | tee "$BUILD_DIR/libevdev_meson.log"

    # Strip libevdev's tools/ CLI programs — they pull in libgen.h and
    # assume Linux-namespaced <linux/input.h>.  We only need the
    # library for libinput to link against.  Delete the `# tools`
    # executable block + its install_man line.  Idempotent.
    if ! grep -q "MAKAOS_NO_TOOLS" "$SRC_DIR/meson.build"; then
        python3 <<PYEOF
p = '$SRC_DIR/meson.build'
with open(p) as f: s = f.read()
marker = '\n# MAKAOS_NO_TOOLS — tools block removed\n'
import re
# Remove the '# tools\n<executable blocks>\ninstall_man(...)' chunk.
new = re.sub(
    r"\n# tools\n.*?install_man\(\s*'tools/[^)]*\)\n",
    marker, s, count=1, flags=re.DOTALL)
if new != s:
    with open(p, 'w') as f: f.write(new)
    print('stripped tools block')
else:
    print('no match — tools block not found')
PYEOF
        rm -rf "$OUT_DIR"
        meson setup "$OUT_DIR" "$SRC_DIR" \
            --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
            --prefix=/usr --libdir=lib \
            -Ddefault_library=static \
            -Dtests=disabled -Ddocumentation=disabled -Dcoverity=false \
            2>&1 | tee -a "$BUILD_DIR/libevdev_meson.log"
    fi
}

build_install() {
    log "ninja build"
    ninja -C "$OUT_DIR" 2>&1 | tee "$BUILD_DIR/libevdev_ninja.log"
    DESTDIR="$SYSROOT" ninja -C "$OUT_DIR" install \
        2>&1 | tee "$BUILD_DIR/libevdev_install.log"
}

main() {
    # Remove the hand-rolled stub if it's still in the sysroot — we
    # want pkg-config + linker to pick up the new one unambiguously.
    rm -f "$SYSROOT/usr/lib/libevdev.a" \
          "$SYSROOT/usr/include/libevdev/libevdev.h" \
          "$SYSROOT/usr/lib/pkgconfig/libevdev.pc"
    fetch
    configure
    build_install
    log "done"
    ls -la "$SYSROOT/usr/lib/libevdev"* 2>/dev/null
}
main "$@"
