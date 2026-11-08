#!/usr/bin/env bash
# Boot MakaOS interactively in a QEMU window with a GPU attached.
#
#   ./scripts/boot.sh            # boot the existing build/disk.img
#   ./scripts/boot.sh --build    # (re)build a fresh disk first, then boot
#
# MakaOS puts its console (and the sway desktop) on the virtio-gpu display.
# QEMU shows the plain VGA head first, so if the window is black, switch to
# the virtio-gpu head:  SDL: Ctrl+Alt+2   GTK: View menu.
#
# Serial (kernel log) is tee'd to build/serial.txt.  Quit with the window's
# close button or  pkill qemu-system-x86  from another terminal.
set -euo pipefail
cd "$(dirname "$0")/.."          # repo root

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"

if [[ "${1:-}" == "--build" ]]; then
    echo "[boot] building a fresh disk (straight to login)..."
    NO_QEMU=1 bash build.sh          # build only; do not launch the headless QEMU
fi

if [[ ! -f build/disk.img ]]; then
    echo "[boot] build/disk.img not found -- run 'bash build.sh' once first (or ./scripts/boot.sh --build)." >&2
    exit 1
fi

[[ -f "$OVMF_CODE" ]] || { echo "[boot] missing $OVMF_CODE (install the 'ovmf' package)." >&2; exit 1; }
cp "$OVMF_VARS_SRC" build/OVMF_VARS.fd

# Display: prefer SDL (it shows the guest cursor); fall back to GTK.  The
# virtio-tablet below makes the pointer ABSOLUTE, so the guest cursor tracks
# your host cursor 1:1 -- always visible, smooth, and unaffected by window
# scaling (a relative PS/2 mouse needed a grab and got jumpy when scaled).
DISPLAY_OPT="sdl"
if ! qemu-system-x86_64 -display help 2>/dev/null | grep -qw sdl; then
    DISPLAY_OPT="gtk"
fi

echo "[boot] starting MakaOS  (display=$DISPLAY_OPT; kernel log -> build/serial.txt)"
echo "[boot] log in (e.g. root / toor).  Fullscreen is Ctrl+Alt+F.  If the window"
echo "[boot] is black, switch to the virtio-gpu head: SDL: Ctrl+Alt+2  GTK: View menu."

pkill qemu-system-x86 2>/dev/null || true
sleep 1
: > build/serial.txt

exec qemu-system-x86_64 \
    -accel kvm -cpu host -smp 4 -m 1024M \
    -nodefaults -no-user-config \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
    -drive format=raw,file=build/disk.img,if=none,id=hd0 \
    -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
    -vga std -device virtio-gpu-pci \
    -device virtio-tablet-pci \
    -display "$DISPLAY_OPT" \
    -k en-us \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial file:build/serial.txt \
    -no-reboot
    # Input: the base PC machine already provides a PS/2 keyboard + mouse, which
    # MakaOS registers as event0/event1 and the text console + sway read.  Do
    # NOT add virtio-keyboard/-tablet: that makes a SECOND keyboard and QEMU may
    # route your keystrokes there while the console listens on PS/2 -> "no input".
