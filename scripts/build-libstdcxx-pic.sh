#!/usr/bin/env bash
# Rebuild the toolchain's libstdc++.a + libsupc++.a as POSITION-INDEPENDENT,
# in place, standalone against the already-installed cross-g++ (no full gcc
# rebuild).  This REPLACES the toolchain's own libstdc++.a/libsupc++.a -- PIC
# code links fine into the static -no-pie apps too, so there is one C++ runtime,
# not a PIC copy on the side.
#
# WHY: a from-source toolchain built with --disable-shared leaves libstdc++/
# libsupc++ non-PIC (R_X86_64_32S), which ld refuses inside a shared object
# (libSDL3.so) or a PIE.  A fresh toolchain built with --with-pic (see
# build-toolchain.sh) already gets this; this script retrofits an existing one
# without the multi-hour compiler rebuild.  libgcc.a is already PIC.
#
# --with-pic makes libtool compile even the static-only objects with -fPIC.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd)"
GCC_SRC="$REPO/build/third_party/gcc-14.2.0"
SYSROOT="$REPO/build/sysroot"
PREFIX="$REPO/toolchain"
TARGET=x86_64-pc-makaos
TCLIB="$PREFIX/$TARGET/lib"                 # where g++ finds libstdc++.a by default
BUILD="$REPO/build/libstdcxx-pic-build"
JOBS="$(nproc)"
export PATH="$PREFIX/bin:$PATH"

[ -f "$SYSROOT/usr/lib/libc.a" ] || { echo "FATAL: build libc first"; exit 1; }
[ -d "$GCC_SRC/libstdc++-v3" ] || { echo "FATAL: gcc source missing at $GCC_SRC"; exit 1; }

# Skip if the toolchain's libstdc++.a is ALREADY PIC (fresh --with-pic toolchain,
# or a prior retrofit).  Probe one code-bearing member for absolute relocs in an
# allocatable section -- that is exactly what would make ld reject it in a .so.
is_nonpic() {
    local a="$1" tmp; tmp="$(mktemp -d)"
    ( cd "$tmp" && ar x "$a" functexcept.o 2>/dev/null ) || true
    local n=0
    [ -f "$tmp/functexcept.o" ] && n=$(readelf -rW "$tmp/functexcept.o" 2>/dev/null | awk '
        /^Relocation section .\.rela\.(text|rodata|data)/{f=1;next}
        /^Relocation section/{f=0}
        f&&/R_X86_64_32S|R_X86_64_32 /{c++} END{print c+0}')
    rm -rf "$tmp"
    [ "${n:-1}" != "0" ]
}
if [ "${FORCE:-0}" != "1" ] && [ -f "$TCLIB/libstdc++.a" ] && ! is_nonpic "$TCLIB/libstdc++.a"; then
    echo "[libstdcxx-pic] toolchain libstdc++.a is already PIC — skip (FORCE=1 to rebuild)"; exit 0
fi

# libstdc++'s standalone configure probes for <gthr-default.h> in ../libgcc to
# decide _GLIBCXX_HAS_GTHREADS.  That header is GENERATED during libgcc's build
# (a copy of gthr-posix.h) -- which a standalone libstdc++ build skips -- so
# detection silently fails and libstdc++ comes out THREAD-UNSAFE (no std::mutex,
# no static-local guards).  Mesa needs real threads, so stage the posix header
# the installed toolchain already generated, at the -I path configure searches.
GTHR="$PREFIX/$TARGET/include/c++/14.2.0/$TARGET/bits/gthr-default.h"
[ -f "$GTHR" ] || { echo "FATAL: installed gthr-default.h not found at $GTHR"; exit 1; }
mkdir -p "$REPO/build/libgcc"
cp "$GTHR" "$REPO/build/libgcc/gthr-default.h"

rm -rf "$BUILD"; mkdir -p "$BUILD"; cd "$BUILD"

"$GCC_SRC/libstdc++-v3/configure" \
    --host="$TARGET" \
    --build="$(gcc -dumpmachine)" \
    --prefix="$PREFIX" \
    --with-sysroot="$SYSROOT" \
    --disable-nls \
    --disable-shared \
    --enable-threads=posix \
    --disable-multilib \
    --disable-libstdcxx-pch \
    --disable-libstdcxx-filesystem-ts \
    --disable-libstdcxx-backtrace \
    --disable-libstdcxx-verbose \
    --with-pic \
    CFLAGS='-g -O2 -fPIC' CXXFLAGS='-g -O2 -fPIC' \
    CC="$TARGET-gcc" CXX="$TARGET-g++" \
    AR="$TARGET-ar" RANLIB="$TARGET-ranlib" \
    CC_FOR_BUILD=gcc CXX_FOR_BUILD=g++

make -j"$JOBS"

# Install OVER the toolchain's own libstdc++.a/libsupc++.a (they are now PIC).
# The build leaves libstdc++.a in src/.libs and libsupc++.a in libsupc++/.libs.
STDCXX="$(find "$BUILD" -path '*/src/.libs/libstdc++.a' | head -1)"
SUPCXX="$(find "$BUILD" -path '*/libsupc++/.libs/libsupc++.a' | head -1)"
[ -f "$STDCXX" ] && [ -f "$SUPCXX" ] || { echo "FATAL: PIC archives not produced"; exit 1; }
cp "$STDCXX" "$TCLIB/libstdc++.a"
cp "$SUPCXX" "$TCLIB/libsupc++.a"
echo "=== replaced the toolchain's libstdc++.a/libsupc++.a with the PIC build ==="
ls -la "$TCLIB/libstdc++.a" "$TCLIB/libsupc++.a"
