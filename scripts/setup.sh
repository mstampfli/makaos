#!/usr/bin/env bash
# One-command setup from a fresh clone (see docs/BUILDING.md):
#
#   bash scripts/setup.sh             cross compiler + Base image
#   bash scripts/setup.sh --qemu      also build QEMU 9.2 (needed without KVM)
#   bash scripts/setup.sh --desktop   also build the desktop and JVM ports
#
# Safe to rerun: finished steps are skipped by the scripts it calls. Note that
# the final build.sh recreates the root file system (files put into the image
# are gone; copy them again with scripts/diskfiles.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

WANT_QEMU=0; WANT_DESKTOP=0
for a in "$@"; do
    case "$a" in
        --qemu) WANT_QEMU=1 ;;
        --desktop) WANT_DESKTOP=1 ;;
        -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $a (--qemu, --desktop)"; exit 1 ;;
    esac
done

step() { echo; echo "== $*"; }
t0=$(date +%s)

# Host packages: the list from docs/BUILDING.md (+ the desktop/JVM extras).
PKGS="build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl xz-utils clang lld nasm gdisk mtools e2fsprogs qemu-system-x86 ovmf meson ninja-build pkg-config python3 xkb-data fonts-dejavu-core"
[ "$WANT_DESKTOP" = 1 ] && PKGS="$PKGS openjdk-17-jdk-headless python3-mako gperf scdoc gettext glslang-tools libcups2-dev libasound2-dev"
missing=""
for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
if [ -n "$missing" ]; then
    echo "Missing host packages. Install them, then rerun this script:"
    echo "  sudo apt install$missing"
    exit 1
fi
mkdir -p build

step "cross compiler (skipped when already built)"
bash scripts/build-toolchain.sh

step "Base image"
NO_QEMU=1 bash build.sh > build/setup-build.log 2>&1 || { tail -30 build/setup-build.log; exit 1; }
echo "build/disk.img ready (log: build/setup-build.log)"

if [ "$WANT_DESKTOP" = 1 ]; then
    step "desktop and JVM ports"
    bash scripts/run-port-chain.sh
    for p in glfw libjpeg libogg libvorbis sdl2-compat darkplaces; do
        bash "scripts/port-$p.sh" > "build/setup-port-$p.log" 2>&1 || { tail -20 "build/setup-port-$p.log"; exit 1; }
        echo "port $p ok"
    done
    JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 bash scripts/port-avian.sh > build/setup-port-avian.log 2>&1
    bash scripts/port-openjdk.sh > build/setup-port-openjdk.log 2>&1
    NO_QEMU=1 bash build.sh > build/setup-build.log 2>&1
    echo "desktop image ready"
fi

if [ "$WANT_QEMU" = 1 ]; then
    step "QEMU 9.2"
    bash scripts/build-qemu.sh
fi

step "can this machine boot it?"
. scripts/qemu-host.sh
if qemu_select_accel 2>/dev/null; then
    echo "yes: $QEMU_BIN ${QEMU_ACCEL_ARGS[*]}"
else
    qemu_select_accel || true
    echo "Fix KVM, or build QEMU 9.2 with: bash scripts/setup.sh --qemu"
fi

echo
echo "done in $(( $(date +%s) - t0 )) s. Next:"
echo "  ./run.sh                                  boot (login root / toor)"
echo "  bash scripts/diskfiles.sh put <files>     copy your programs in (MakaOS shut down)"
echo "  NO_QEMU=1 bash build.sh                   rebuild after code changes (wipes the disk)"
