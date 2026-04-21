#!/usr/bin/env bash
# ── MakaOS harfbuzz port (cross-g++ + sysroot) ────────────────────────
#
# Complex-script text shaping.  C++ library — first real test of our
# freestanding libstdc++.  Minimal build: just the unified C++
# aggregator (hb.cc).  No Graphite2, no ICU, no GObject.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HB_VERSION="8.5.0"
HB_URL="https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION}/harfbuzz-${HB_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
HB_SRC="$THIRD_PARTY/harfbuzz-${HB_VERSION}"
HB_TARBALL="$THIRD_PARTY/harfbuzz-${HB_VERSION}.tar.xz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CXX="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-g++"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CXX" ]; then
    echo "[port-hb] FATAL: cross-g++ missing (need C++ toolchain)" >&2
    exit 1
fi

CXXFLAGS=(
    -O2 -fPIC
    -std=c++17
    -ffreestanding
    -fno-exceptions -fno-rtti
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-class-memaccess
    -Wno-deprecated-copy
    -Wno-cast-function-type
    # Feature gates — leave unset to disable (defined() trap)
    -DHAVE_PTHREAD=1
    -DHAVE_FREETYPE=1
    -DHB_NO_MT              # no pthread-based shared state — fine for
                             # initial port; wlroots text is single-threaded
    -DHB_NO_GETENV           # disable runtime tunables via env
    -DHB_NO_MMAP             # load fonts via stdio, not mmap
    -I "$SYSROOT/usr/include/freetype2"
)

log() { printf '[port-hb] %s\n' "$*" >&2; }

fetch_hb() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$HB_TARBALL" ]; then
        log "downloading harfbuzz ${HB_VERSION}"
        curl -fSL --retry 3 -o "$HB_TARBALL" "$HB_URL"
    fi
    if [ ! -d "$HB_SRC" ]; then
        log "extracting harfbuzz"
        tar -xJf "$HB_TARBALL" -C "$THIRD_PARTY"
    fi
}

install_headers() {
    log "installing public headers into $SYSROOT/usr/include/harfbuzz"
    mkdir -p "$SYSROOT/usr/include/harfbuzz"
    # All hb-*.h in src/ that don't start with underscore are public.
    for h in "$HB_SRC/src/"hb-*.h; do
        [ -f "$h" ] || continue
        cp "$h" "$SYSROOT/usr/include/harfbuzz/"
    done
    # hb-ft.h for freetype integration (harfbuzz ↔ freetype glue)
    [ -f "$HB_SRC/src/hb-ft.h" ] && cp "$HB_SRC/src/hb-ft.h" "$SYSROOT/usr/include/harfbuzz/"
}

build_lib() {
    local build_objs="$BUILD_DIR/hb_objs"
    mkdir -p "$build_objs"

    local includes=(
        -I "$HB_SRC/src"
    )

    # harfbuzz ships an "amalgamated" aggregator — harfbuzz.cc —
    # that #includes every other .cc.  Single TU means we get the
    # entire library from one compile.
    local src="$HB_SRC/src/harfbuzz.cc"
    local obj="$build_objs/harfbuzz.o"
    if [ "$src" -nt "$obj" ]; then
        log "compiling harfbuzz.cc (unified build)"
        "$CROSS_CXX" "${CXXFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
    fi

    local lib="$SYSROOT/usr/lib/libharfbuzz.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "$obj"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/hb_objs" "$SYSROOT/usr/lib/libharfbuzz.a"
    fi
    fetch_hb
    install_headers
    build_lib
}

main "$@"
