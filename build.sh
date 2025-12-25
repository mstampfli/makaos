#!/usr/bin/env bash
set -e

# directories
BOOT_DIR="bootloader"
LOADER_DIR="kernelLoader"
KERNEL_DIR="kernel"
BUILD_DIR="build"

mkdir -p "$BUILD_DIR"

echo "[+] Building kernelLoader (C/ASM → ELF → BIN)"

CFLAGS=(
  -ffreestanding
  -m64
  -mno-red-zone
  -mgeneral-regs-only
  -fno-pie -fno-pic -fno-plt
  -fno-stack-protector
  -fno-builtin
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-omit-frame-pointer
  -O0
  -g
  -Wall -Wextra -Wpedantic
  -Wno-unused-parameter
  -Wno-missing-field-initializers
)

# ---- build kernelLoader ----
LOADER_C_OBJS=()
for src in "$LOADER_DIR"/*.c; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/loader_$(basename "${src%.c}").o"
  gcc "${CFLAGS[@]}" -c "$src" -o "$obj"
  LOADER_C_OBJS+=("$obj")
done

LOADER_ASM_OBJS=()
for src in "$LOADER_DIR"/*.asm; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/loader_$(basename "${src%.asm}").o"
  nasm -f elf64 -g -F dwarf "$src" -o "$obj"
  LOADER_ASM_OBJS+=("$obj")
done

# link.ld is now in repo root
ld -nostdlib -z max-page-size=0x1000 -T "$LOADER_DIR/link.ld" \
  "${LOADER_ASM_OBJS[@]}" "${LOADER_C_OBJS[@]}" \
  -o "$BUILD_DIR/kernelLoader.elf"

objcopy -O binary \
  -j .text -j .rodata -j .data -j .bss \
  "$BUILD_DIR/kernelLoader.elf" "$BUILD_DIR/kernelLoader.bin"


echo "[+] Building kernel (C/ASM → ELF → BIN)"

# ---- build kernel (the one loaded by kernelLoader) ----
KERNEL_C_OBJS=()
for src in "$KERNEL_DIR"/*.c; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/kernel_$(basename "${src%.c}").o"
  gcc "${CFLAGS[@]}" -c "$src" -o "$obj"
  KERNEL_C_OBJS+=("$obj")
done

KERNEL_ASM_OBJS=()
for src in "$KERNEL_DIR"/*.asm; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/kernel_$(basename "${src%.asm}").o"
  nasm -f elf64 -g -F dwarf "$src" -o "$obj"
  KERNEL_ASM_OBJS+=("$obj")
done

ld -nostdlib -z max-page-size=0x1000 -T "$KERNEL_DIR/link.ld" \
  "${KERNEL_ASM_OBJS[@]}" "${KERNEL_C_OBJS[@]}" \
  -o "$BUILD_DIR/kernel.elf"

objcopy -O binary \
  -j .text -j .rodata -j .data -j .bss \
  "$BUILD_DIR/kernel.elf" "$BUILD_DIR/kernel.bin"


echo "[+] Building bootloaders (NASM)"

nasm -f bin "$BOOT_DIR/boot1.asm" -o "$BUILD_DIR/boot1.bin"
nasm -f bin "$BOOT_DIR/boot2.asm" -o "$BUILD_DIR/boot2.bin"

echo "[+] Checking sizes"
stat -c "%n %s" \
  "$BUILD_DIR/boot1.bin" \
  "$BUILD_DIR/boot2.bin" \
  "$BUILD_DIR/kernelLoader.bin" \
  "$BUILD_DIR/kernel.bin"

echo "[+] Checking boot signature (must be 55aa)"
tail -c 2 "$BUILD_DIR/boot1.bin" | xxd -p

# Pad boot2 to 16 KiB (32 sectors)
truncate -s 16384 "$BUILD_DIR/boot2.bin"

echo "[+] Creating disk image"

# Fixed layout (for now):
# LBA 0           : boot1 (1 sector)
# LBA 1–32        : boot2 (32 sectors)
# LBA 33–(33+N1)  : kernelLoader.bin
# LBA 2048+       : kernel.bin  (1 MiB offset, easy to reason about)
#
# 2048 sectors * 512 = 1 MiB

LOADER_LBA=33
KERNEL_LBA=2048

# big enough for loader+kernel growth
DISK_SECTORS=$((KERNEL_LBA + 32768))   # +16 MiB after kernel start

truncate -s $((DISK_SECTORS * 512)) "$BUILD_DIR/disk.img"

dd if="$BUILD_DIR/boot1.bin"        of="$BUILD_DIR/disk.img" bs=512 seek=0          conv=notrunc status=none
dd if="$BUILD_DIR/boot2.bin"        of="$BUILD_DIR/disk.img" bs=512 seek=1          conv=notrunc status=none
dd if="$BUILD_DIR/kernelLoader.bin" of="$BUILD_DIR/disk.img" bs=512 seek=$LOADER_LBA conv=notrunc status=none
dd if="$BUILD_DIR/kernel.bin"       of="$BUILD_DIR/disk.img" bs=512 seek=$KERNEL_LBA conv=notrunc status=none

stat -c "%n %s" "$BUILD_DIR/disk.img"

echo "[+] Running QEMU"

qemu-system-x86_64 \
  -drive format=raw,file=build/disk.img,if=ide \
  -serial mon:stdio \
  -no-reboot \
  -no-shutdown
