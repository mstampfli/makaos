#!/usr/bin/env bash
# Build QEMU 9.2.0 (x86_64 system emulator, with SDL/GTK windows, slirp
# networking, PulseAudio and virgl 3D) into ~/opt/qemu-9.2.0.
#
# Only needed without KVM: MakaOS requires x2APIC, which QEMU's software
# emulation provides from 9.0 on (Ubuntu 24.04 ships 8.2). scripts/qemu-host.sh
# finds this install by itself. About 10 minutes.
set -euo pipefail

V=9.2.0
SHA256=f859f0bc65e1f533d040bbe8c92bcfecee5af2c921a6687c652fb44d089bd894
PREFIX="${PREFIX:-$HOME/opt/qemu-$V}"
WORK="${WORK:-$HOME/src}"
DEPS="libglib2.0-dev libpixman-1-dev libslirp-dev libsdl2-dev libpulse-dev libvirglrenderer-dev libepoxy-dev libgbm-dev ninja-build python3-venv pkg-config flex bison"

missing=""
for p in $DEPS; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
if [ -n "$missing" ]; then
    echo "[build-qemu] missing host packages; install them with:"
    echo "  sudo apt install$missing"
    exit 1
fi

if [ -x "$PREFIX/bin/qemu-system-x86_64" ]; then
    echo "[build-qemu] already installed: $("$PREFIX/bin/qemu-system-x86_64" --version | head -1)"
    exit 0
fi

mkdir -p "$WORK"
cd "$WORK"
[ -f "qemu-$V.tar.xz" ] || curl -fSL --retry 3 -o "qemu-$V.tar.xz" "https://download.qemu.org/qemu-$V.tar.xz"
echo "$SHA256  qemu-$V.tar.xz" | sha256sum -c - || { echo "[build-qemu] checksum mismatch"; exit 1; }
[ -d "qemu-$V" ] || tar -xJf "qemu-$V.tar.xz"
cd "qemu-$V"
./configure --prefix="$PREFIX" --target-list=x86_64-softmmu \
    --disable-docs --disable-werror --enable-slirp --enable-sdl \
    --enable-opengl --enable-virglrenderer > configure.log 2>&1 \
    || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -40 make.log; exit 1; }
make install > install.log 2>&1
"$PREFIX/bin/qemu-system-x86_64" --version | head -1
