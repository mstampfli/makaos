#!/usr/bin/env bash
# ── MakaOS libxkbcommon port (cross-gcc + sysroot) ────────────────────
#
# libxkbcommon is the wayland keyboard layout library.  Plain C, only
# real dependency is the xkeyboard-config keymap files (we install a
# minimal default).  Skips libX11 (x11/ subdir not built), skips the
# new xkbregistry tool, no ICU dependency.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XKB_VERSION="1.7.0"
XKB_URL="https://xkbcommon.org/download/libxkbcommon-${XKB_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
XKB_SRC="$THIRD_PARTY/libxkbcommon-${XKB_VERSION}"
XKB_TARBALL="$THIRD_PARTY/libxkbcommon-${XKB_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-xkb] FATAL: cross-toolchain missing" >&2
    exit 1
fi

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
    -DDFLT_XKB_CONFIG_ROOT='"/usr/share/X11/xkb"'
    -DDEFAULT_XKB_RULES='"evdev"'
    -DDEFAULT_XKB_MODEL='"pc105"'
    -DDEFAULT_XKB_LAYOUT='"us"'
)

log() { printf '[port-xkb] %s\n' "$*" >&2; }

fetch_xkb() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$XKB_TARBALL" ]; then
        log "downloading libxkbcommon ${XKB_VERSION}"
        curl -fSL --retry 3 -o "$XKB_TARBALL" "$XKB_URL"
    fi
    if [ ! -d "$XKB_SRC" ]; then
        log "extracting libxkbcommon"
        tar -xJf "$XKB_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/xkbcommon"
    mkdir -p "$SYSROOT/usr/include/xkbcommon"
    cp "$XKB_SRC/include/xkbcommon/"*.h "$SYSROOT/usr/include/xkbcommon/"
}

build_lib() {
    local build_objs="$BUILD_DIR/xkb_objs"
    mkdir -p "$build_objs"

    # Drop the config.h every TU expects to include.
    cp "$REPO_ROOT/scripts/configs/xkbcommon_config.h" "$XKB_SRC/config.h"

    # MakaOS: MAP_SHARED on regular-fd mmap not yet supported (needs
    # dirty-tracking + writeback).  xkbcommon only reads its mapping
    # PROT_READ, so MAP_PRIVATE is functionally identical.  Idempotent.
    python3 -c "
p='$XKB_SRC/src/utils.c'
with open(p) as f: s = f.read()
if 'MAP_SHARED' in s and 'MAKAOS_MAP_PRIVATE' not in s:
    s = s.replace('MAP_SHARED', '/*MAKAOS_MAP_PRIVATE*/MAP_PRIVATE')
    with open(p, 'w') as f: f.write(s)
    print('patched utils.c: MAP_SHARED -> MAP_PRIVATE')
"

    # Generate parser.{h,c} from parser.y via bison — meson would
    # normally do this.  Output header next to parser-priv.h.
    if [ ! -f "$XKB_SRC/src/xkbcomp/parser.h" ]; then
        (cd "$XKB_SRC/src/xkbcomp" && \
         bison --name-prefix=_xkbcommon_ -d -o parser.c parser.y)
    fi

    local includes=(
        -I "$XKB_SRC"
        -I "$XKB_SRC/include"
        -I "$XKB_SRC/src"
        -I "$XKB_SRC/src/xkbcomp"
    )

    # Sources: core xkb library, NOT the X11 wrapper (skip src/x11/) or
    # xkbregistry (needs libxml2).  This is the wlroots-needed subset.
    local srcs=(
        "src/atom.c"
        "src/context.c"
        "src/context-priv.c"
        "src/keysym.c"
        "src/keysym-utf.c"
        "src/keymap.c"
        "src/keymap-priv.c"
        "src/state.c"
        "src/text.c"
        "src/utf8.c"
        "src/utils.c"
        "src/util-list.c"
        "src/xkbcomp/action.c"
        "src/xkbcomp/ast-build.c"
        "src/xkbcomp/compat.c"
        "src/xkbcomp/expr.c"
        "src/xkbcomp/include.c"
        "src/xkbcomp/keycodes.c"
        "src/xkbcomp/keymap-dump.c"
        "src/xkbcomp/keymap.c"
        "src/xkbcomp/keywords.c"
        "src/xkbcomp/parser.c"
        "src/xkbcomp/rules.c"
        "src/xkbcomp/scanner.c"
        "src/xkbcomp/symbols.c"
        "src/xkbcomp/types.c"
        "src/xkbcomp/vmod.c"
        "src/xkbcomp/xkbcomp.c"
        "src/compose/parser.c"
        "src/compose/paths.c"
        "src/compose/state.c"
        "src/compose/table.c"
    )

    log "compiling ${#srcs[@]} libxkbcommon sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$XKB_SRC/$name"
        # Flatten tree path into obj name — src/keymap.c and
        # src/xkbcomp/keymap.c must not collide as "keymap.o" in ar.
        local flat="${name//\//_}"
        local obj="$build_objs/${flat%.c}.o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libxkbcommon.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/xkb_objs" "$SYSROOT/usr/lib/libxkbcommon.a"
    fi
    fetch_xkb
    install_headers
    build_lib
}

main "$@"
