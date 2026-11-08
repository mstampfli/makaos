#!/usr/bin/env bash
# Headless boot that autologins into a target binary (default /bin/sdltest),
# which signals its result via a PF-KILL sentinel (sdltest full pass=0x5D10001F,
# dltest full pass=0x5EC03FFF).  Set BIN=/bin/dltest to run the regression test.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR=build
EXT2_LBA=4096
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"
TIMEOUT="${TIMEOUT:-150}"
BIN="${BIN:-/bin/sdltest}"

pkill -9 -f qemu-system-x86_64 2>/dev/null || true
sleep 3

# Inject /etc/autologin -> root:$BIN into the ext2 image, then re-splice it into
# the GPT disk (same dd the build uses).  Non-destructive to everything else.
# Done with NO qemu running so the concurrent-write cannot corrupt disk.img.
printf 'root:%s\n' "$BIN" > "$BUILD_DIR/etc_stage_autologin"
debugfs -w "$BUILD_DIR/ext2.img" -R "rm /etc/autologin" >/dev/null 2>&1 || true
debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage_autologin etc/autologin" >/dev/null 2>&1 || true
debugfs -w "$BUILD_DIR/ext2.img" -R "set_inode_field /etc/autologin mode 0100600" >/dev/null 2>&1 || true
debugfs -w "$BUILD_DIR/ext2.img" -R "set_inode_field /etc/autologin uid 0" >/dev/null 2>&1 || true
dd if="$BUILD_DIR/ext2.img" of="$BUILD_DIR/disk.img" bs=512 seek=$EXT2_LBA conv=notrunc status=none
cp "$OVMF_VARS_SRC" "$OVMF_VARS"   # fresh UEFI NVRAM, same as build.sh
echo "[run] autologin -> root:$BIN injected + disk re-spliced"

: > "$BUILD_DIR/serial.txt"

timeout "$TIMEOUT" qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 1024M \
  -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file="$BUILD_DIR/disk.img",if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -device virtio-gpu-pci -display none \
  -serial file:"$BUILD_DIR/serial.txt" \
  -monitor none -no-reboot -no-shutdown >/dev/null 2>&1 || true

echo "[run] QEMU exited (timeout=${TIMEOUT}s)"
echo "=== sdltest PF-KILL sentinel(s) in serial.txt ==="
grep -iE 'PF-KILL|CR2|sdltest|panic' "$BUILD_DIR/serial.txt" | tail -20 || echo "  (none found)"
