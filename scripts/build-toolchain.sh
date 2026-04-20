#!/usr/bin/env bash
# ── MakaOS cross-toolchain builder ─────────────────────────────────────
#
# Produces a full binutils + gcc cross-compiler targeting
# x86_64-pc-makaos, installed at $REPO_ROOT/toolchain/.  After this
# runs, external projects can cross-compile with the standard autotools
# dance:
#     ./configure --host=x86_64-pc-makaos --prefix=$SYSROOT/usr
#     make && make install
#
# First run: ~30–60 minutes.  Subsequent runs are no-ops unless
# FORCE=1 is passed.  Patches under scripts/patches/{binutils,gcc}.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
PATCHES_DIR="$REPO_ROOT/scripts/patches"

BINUTILS_VERSION="2.42"
GCC_VERSION="14.2.0"

TARGET="x86_64-pc-makaos"
PREFIX="$REPO_ROOT/toolchain"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
JOBS="$(nproc)"

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"

BINUTILS_SRC="$THIRD_PARTY/binutils-${BINUTILS_VERSION}"
GCC_SRC="$THIRD_PARTY/gcc-${GCC_VERSION}"

log() { printf '\033[1;36m[toolchain]\033[0m %s\n' "$*" >&2; }

# ── Prerequisites (debian/ubuntu names) ────────────────────────────────
check_host_prereqs() {
    local missing=()
    for t in curl tar make gcc g++ bison flex patch makeinfo; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        log "missing host tools: ${missing[*]}"
        log "install: sudo apt install build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev"
        exit 1
    fi
}

# ── Fetch + extract + patch (idempotent) ───────────────────────────────
fetch_and_patch() {
    local name="$1" url="$2" src="$3" tar="$4" patches="$5"
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$tar" ]; then
        log "downloading $name"
        curl -fSL --retry 3 -o "$tar" "$url"
    fi
    if [ ! -d "$src" ]; then
        log "extracting $name"
        tar -xf "$tar" -C "$THIRD_PARTY"
    fi
    if [ -d "$patches" ] && [ ! -f "$src/.makaos-patched" ]; then
        log "patching $name"
        for p in "$patches"/*.patch; do
            [ -f "$p" ] || continue
            patch -d "$src" -p1 < "$p"
        done
        touch "$src/.makaos-patched"
    fi
}

# ── Build binutils ─────────────────────────────────────────────────────
build_binutils() {
    local build="$BUILD_DIR/binutils-${BINUTILS_VERSION}-build"
    if [ -f "$PREFIX/bin/${TARGET}-ld" ] && [ "${FORCE:-0}" != "1" ]; then
        log "binutils already built — skip (FORCE=1 to rebuild)"
        return
    fi
    log "configuring binutils"
    rm -rf "$build"
    mkdir -p "$build"
    (cd "$build" && \
        "$BINUTILS_SRC/configure" \
            --target="$TARGET" \
            --prefix="$PREFIX" \
            --with-sysroot="$SYSROOT" \
            --disable-nls \
            --disable-werror \
            --disable-multilib)
    log "building binutils (this takes ~5 min)"
    make -C "$build" -j"$JOBS"
    log "installing binutils"
    make -C "$build" install
}

# ── Build gcc stage 1 (cross-compiler + libgcc) ────────────────────────
build_gcc() {
    local build="$BUILD_DIR/gcc-${GCC_VERSION}-build"
    if [ -f "$PREFIX/bin/${TARGET}-gcc" ] && [ "${FORCE:-0}" != "1" ]; then
        log "gcc already built — skip (FORCE=1 to rebuild)"
        return
    fi
    log "fetching gcc prerequisites (gmp/mpfr/mpc/isl)"
    (cd "$GCC_SRC" && ./contrib/download_prerequisites)

    log "configuring gcc (C + C++)"
    # Sysroot must already contain a built libc.a + split headers; the
    # top-level ./build.sh produces those.  libstdc++ autoconf needs
    # them to link its probe test cases during configure.
    if [ ! -f "$SYSROOT/usr/lib/libc.a" ]; then
        log "FATAL: $SYSROOT/usr/lib/libc.a missing — run ./build.sh first"
        exit 1
    fi
    if [ ! -f "$SYSROOT/usr/include/pthread.h" ]; then
        log "FATAL: $SYSROOT/usr/include/pthread.h missing — run ./build.sh first"
        exit 1
    fi

    rm -rf "$build"
    mkdir -p "$build"
    # Put the newly-built x86_64-pc-makaos-{as,ld,ar} on PATH so gcc's
    # configure finds them.
    export PATH="$PREFIX/bin:$PATH"
    (cd "$build" && \
        "$GCC_SRC/configure" \
            --target="$TARGET" \
            --prefix="$PREFIX" \
            --with-sysroot="$SYSROOT" \
            --with-native-system-header-dir=/usr/include \
            --disable-nls \
            --enable-languages=c,c++ \
            --disable-shared \
            --enable-threads=posix \
            --disable-libssp \
            --disable-libmudflap \
            --disable-libgomp \
            --disable-libquadmath \
            --disable-libatomic \
            --disable-multilib \
            --disable-hosted-libstdcxx \
            --disable-wchar_t \
            --disable-libstdcxx-pch \
            --disable-libstdcxx-filesystem-ts \
            --disable-libstdcxx-backtrace \
            --disable-libstdcxx-verbose)
    log "building gcc"
    make -C "$build" -j"$JOBS" all-gcc
    log "building libgcc"
    make -C "$build" -j"$JOBS" all-target-libgcc
    log "installing gcc + libgcc (needed before libstdc++ can probe)"
    make -C "$build" install-gcc install-target-libgcc
    log "building libstdc++"
    # Freestanding libstdc++: headers + type traits + <atomic> + exceptions,
    # no <iostream>/<fstream>/<regex>/<random> (those need full hosted libc
    # features we don't all ship).  Ladybird and most C++ libs compile
    # against the hosted bits they actually use; the unused headers simply
    # aren't present.  If a port fails on a missing header we add a shim.
    make -C "$build" -j"$JOBS" all-target-libstdc++-v3
    log "installing libstdc++"
    make -C "$build" install-target-libstdc++-v3
}

main() {
    check_host_prereqs
    fetch_and_patch "binutils ${BINUTILS_VERSION}" \
        "$BINUTILS_URL" "$BINUTILS_SRC" \
        "$THIRD_PARTY/binutils-${BINUTILS_VERSION}.tar.xz" \
        "$PATCHES_DIR/binutils"
    fetch_and_patch "gcc ${GCC_VERSION}" \
        "$GCC_URL" "$GCC_SRC" \
        "$THIRD_PARTY/gcc-${GCC_VERSION}.tar.xz" \
        "$PATCHES_DIR/gcc"
    build_binutils
    build_gcc
    log "done — cross-toolchain at $PREFIX/bin/${TARGET}-{gcc,ld,as,ar,...}"
}

main "$@"
