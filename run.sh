#!/usr/bin/env bash
set -e

BUILD_DIR="build"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"

cp "$OVMF_VARS_SRC" "$OVMF_VARS"

qemu-system-x86_64 \
  -accel kvm \
  -cpu host \
  -smp 4 \
  -m 256M \
  -nodefaults \
  -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file="$BUILD_DIR/disk.img",if=none,id=hd0 \
  -device ahci,id=ahci \
  -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga none \
  -device virtio-vga,xres=1280,yres=800 \
  -display sdl \
  -audiodev pa,id=snd0 \
  -device intel-hda \
  -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0,hostfwd=tcp::18080-:80 \
  -device virtio-net-pci,netdev=net0 \
  -serial file:build/serial.txt \
  -monitor unix:/tmp/qemu-hmp.sock,server,nowait \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown
