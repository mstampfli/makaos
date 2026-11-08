#!/usr/bin/env bash
# Interactive sway boot in an SDL window, using the known-good display combo
# (-vga std + virtio-gpu-pci).  Boots the CURRENT build/disk.img as-is, so
# whatever was last built (kernel + autologin) is what runs.
set -e
cd "$(dirname "$0")"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.fd

exec qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 1024M -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=build/disk.img,if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -device virtio-gpu-pci \
  -device virtio-tablet-pci -device virtio-keyboard-pci \
  -display sdl \
  -serial file:build/serial.txt \
  -no-reboot
