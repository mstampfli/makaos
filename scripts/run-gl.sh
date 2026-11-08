#!/usr/bin/env bash
# Boot MakaOS under QEMU with the virtio-gpu 3D (virgl) device enabled, for the
# virgl bring-up (docs/VIRGL_BRINGUP.md).  This is the gl-enabled launch VARIANT
# -- build.sh / run.sh keep using plain virtio-gpu-pci (2D) as the default, so
# the working desktop is never affected.
#
# Requires a virgl-capable host: QEMU built with virglrenderer, a usable render
# node (default /dev/dri/renderD128), and host EGL.  Verify with:
#   qemu-system-x86_64 -device help | grep virtio-gpu-gl
#   ldconfig -p | grep virgl
#
# Today this exercises the Phase 0 capset probe (kernel logs the host's virgl
# capsets at virtio-gpu init).  Later phases drive real 3D through the same
# device.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

RENDERNODE="${RENDERNODE:-/dev/dri/renderD128}"
SERIAL="${SERIAL:-build/serial-gl.txt}"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS_gl.fd"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$OVMF_VARS"

pkill -9 -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true

echo "[run-gl] virtio-gpu-gl + egl-headless (rendernode=$RENDERNODE); serial -> $SERIAL"
exec qemu-system-x86_64 \
  -accel kvm -cpu host -smp "${QEMU_SMP:-4}" -m "${QEMU_MEM:-1024M}" -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file=build/disk.img,if=none,id=hd0,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -device virtio-gpu-gl-pci,id=vgpu \
  -display egl-headless,rendernode="$RENDERNODE" \
  -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -serial file:"$SERIAL" \
  -no-reboot -no-shutdown \
  "$@"
