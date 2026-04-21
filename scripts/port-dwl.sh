#!/usr/bin/env bash
# ── MakaOS dwl port ────────────────────────────────────────────────────
#
# dwl is a suckless-style Wayland compositor in ~3 KLOC of C.  Its only
# "problem" dependency on our stack is libinput: dwl uses it exclusively
# for laptop-touchpad config (tap-to-click, natural-scroll, etc.) inside
# a single `if (wlr_input_device_is_libinput(...))` block.  Since we
# disabled wlroots' libinput backend, that runtime check always returns
# false — but the block still needs to COMPILE.
#
# We patch dwl to remove the libinput dependency entirely.  Keyboard
# and pointer input flow through our native wlroots backend unchanged;
# only the touchpad-config fine-tuning is gone (not relevant without
# a real touchpad anyway).

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
    log "patching dwl: strip libinput-only code, stub wlr_session_change_vt"

    # config.h is derived from config.def.h; make sure it exists AND
    # pulls in our libinput-shim (enums only) for the config variables.
    if [ ! -f "$DWL_SRC/config.h" ]; then
        cp "$DWL_SRC/config.def.h" "$DWL_SRC/config.h"
    fi
    # Prepend <libinput.h> include to config.h so the enum type decls
    # on static-file-scope variables resolve.
    if ! head -5 "$DWL_SRC/config.h" | grep -q "libinput.h"; then
        { printf '#include <libinput.h>\n'; cat "$DWL_SRC/config.h"; } > "$DWL_SRC/config.h.new"
        mv "$DWL_SRC/config.h.new" "$DWL_SRC/config.h"
    fi

    python3 - <<PY
import re, sys
path = "$DWL_SRC/dwl.c"
with open(path) as h: s = h.read()
# 1. Drop libinput backend include (keep config.h's <libinput.h> enum
#    shim; the wlr/backend/libinput.h one is a real runtime dep we
#    don't have).
s = re.sub(r'^#include <libinput.h>\n',            '', s, flags=re.M)
s = re.sub(r'^#include <wlr/backend/libinput.h>\n', '', s, flags=re.M)
# 2. Remove the per-pointer libinput-config block.  The block starts
#    at "struct libinput_device *device;" and runs through the closing
#    brace of the enclosing if.
old = re.search(
    r'\tstruct libinput_device \*device;\n'
    r'\tif \(wlr_input_device_is_libinput\(&pointer->base\).*?\n\t\}\n',
    s, flags=re.DOTALL)
assert old, "libinput block not located"
s = s[:old.start()] + '\t/* MakaOS: libinput-config block removed (native input backend). */\n' + s[old.end():]
# 3. Replace wlr_session_change_vt calls with a no-op — session backend
#    is disabled in our wlroots build; VT switching doesn't exist on
#    MakaOS anyway.
s = s.replace('wlr_session_change_vt(session,', '(void)(0 &&')
# 4. First-line marker.
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
