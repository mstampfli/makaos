#!/usr/bin/env bash
# ── MakaOS dwl port ────────────────────────────────────────────────────
#
# dwl is a suckless-style Wayland compositor in ~3 KLOC of C.  We now
# build it unmodified against the real wlroots libinput backend
# (MakaOS ports libinput + libevdev + mtdev).  The only patches left
# are compositor-environment workarounds:
#
#  * stub out wlr_session_change_vt — MakaOS has no VTs
#  * XDG_RUNTIME_DIR fallback to /tmp — SYS_EXEC drops envp today

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DWL_VERSION="0.7"
DWL_URL="https://codeberg.org/dwl/dwl/archive/v${DWL_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
DWL_SRC="$THIRD_PARTY/dwl"
DWL_TARBALL="$THIRD_PARTY/dwl-${DWL_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
TC_INCLUDE="$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include"

export PATH="$REPO_ROOT/build/host-tools/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

log() { printf '[port-dwl] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$DWL_TARBALL" ] || curl -fsSL -o "$DWL_TARBALL" "$DWL_URL"
    [ -d "$DWL_SRC" ]     || tar -xzf "$DWL_TARBALL" -C "$THIRD_PARTY"
}

patch_dwl() {
    if grep -q "MAKAOS_PATCHED" "$DWL_SRC/dwl.c" 2>/dev/null; then
        log "dwl already patched"; return 0
    fi
    log "patching dwl: stub wlr_session_change_vt + XDG_RUNTIME_DIR fallback"

    # config.h is derived from config.def.h — create if missing.
    # (Unlike the earlier libinput-skip port, config.h doesn't need a
    # prepended <libinput.h> here: dwl.c already includes it via
    # wlr/backend/libinput.h, and libinput.h is on the sysroot include
    # path.  Keep the copy step idempotent.)
    if [ ! -f "$DWL_SRC/config.h" ]; then
        cp "$DWL_SRC/config.def.h" "$DWL_SRC/config.h"
    fi

    python3 - <<PY
import re
path = "$DWL_SRC/dwl.c"
with open(path) as h: s = h.read()
# 1. Replace wlr_session_change_vt calls with a no-op — MakaOS has no
#    VTs, so the session backend never dispatches VT switches.
s = s.replace('wlr_session_change_vt(session,', '(void)(0 &&')
# 2. Default XDG_RUNTIME_DIR — our kernel's SYS_EXEC drops envp, so
#    dwl's own die() guard needs to fall back to /tmp.
s = s.replace(
    'if (!getenv("XDG_RUNTIME_DIR"))\\n\\t\\tdie("XDG_RUNTIME_DIR must be set");',
    'if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);')
s = s.replace(
    'if (!getenv("XDG_RUNTIME_DIR"))\n\t\tdie("XDG_RUNTIME_DIR must be set");',
    'if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);')
# 3. First-line marker.
s = "/* MAKAOS_PATCHED — see scripts/port-dwl.sh */\n" + s
with open(path, 'w') as h: h.write(s)
print("patched")
PY
}

build() {
    local cflags=(
        -O2 -std=gnu11
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-missing-field-initializers
        -DWLR_USE_UNSTABLE
        '-DVERSION="'"${DWL_VERSION}"'"'
        --sysroot="$SYSROOT"
        -nostdinc
        -isystem "$SYSROOT/usr/include"
        -isystem "$TC_INCLUDE"
        -I "$SYSROOT/usr/include/wlroots-0.18"
        -I "$SYSROOT/usr/include/pixman-1"
        -I "$SYSROOT/usr/include/libdrm"
    )

    # The protocol subdir of dwl has XML files wlroots' scanner must turn
    # into C headers.  Generate what dwl #includes.
    local pdir="$DWL_SRC/protocols"
    local scanner="$REPO_ROOT/build/host-tools/bin/wayland-scanner"
    for xml in "$pdir"/*.xml; do
        [ -f "$xml" ] || continue
        local base=$(basename "$xml" .xml)
        [ -f "$pdir/${base}-protocol.h" ] || \
            "$scanner" server-header "$xml" "$pdir/${base}-protocol.h"
        [ -f "$pdir/${base}-protocol.c" ] || \
            "$scanner" private-code   "$xml" "$pdir/${base}-protocol.c"
    done

    (cd "$DWL_SRC" && \
        "$CROSS_CC" "${cflags[@]}" -I "$pdir" \
        -nostartfiles -Wl,--build-id=none \
        "$SYSROOT/usr/lib/crt0.o" \
        dwl.c util.c \
        -Wl,--start-group \
        -lwlroots-0.18 -lwayland-server -lwayland-client \
        -lxkbcommon -lpixman-1 -ldrm -ludev -lseat -lffi -ldisplay-info \
        -linput -levdev -lmtdev \
        -lc -lm -lrt -lpthread -ldl \
        -Wl,--end-group \
        -o dwl.elf)

    mkdir -p "$SYSROOT/usr/bin"
    cp "$DWL_SRC/dwl.elf" "$SYSROOT/usr/bin/dwl"
    log "dwl.elf: $(stat -c%s "$DWL_SRC/dwl.elf") bytes"
}

main() {
    fetch
    patch_dwl
    build
    log "done — $SYSROOT/usr/bin/dwl"
}
main "$@"
