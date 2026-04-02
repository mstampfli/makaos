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
  -fno-omit-frame-pointer -mcmodel=kernel
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


echo "[+] Building user binaries"

USER_DIR="user"
USER_CFLAGS=(
  -ffreestanding -m64 -mno-red-zone
  -fno-pie -fno-pic -fno-plt
  -fno-stack-protector -fno-builtin
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -O0 -Wall -Wextra
  -Wno-unused-parameter
)

nasm -f elf64 "$USER_DIR/entry.asm" -o "$BUILD_DIR/user_entry.o"
gcc "${USER_CFLAGS[@]}" -c "$USER_DIR/test_vmalloc.c" -o "$BUILD_DIR/user_test_vmalloc.o"
ld -nostdlib -T "$USER_DIR/user_link.ld" "$BUILD_DIR/user_entry.o" "$BUILD_DIR/user_test_vmalloc.o" \
   -o "$BUILD_DIR/user_test_vmalloc.elf"
objcopy -O binary "$BUILD_DIR/user_test_vmalloc.elf" "$BUILD_DIR/user_test_vmalloc.bin"

# Generate a C header that embeds the binary as a byte array.
BIN="$BUILD_DIR/user_test_vmalloc.bin"
SZ=$(stat -c "%s" "$BIN")
{
  echo "#pragma once"
  echo "static const unsigned char g_user_test_vmalloc[] = {"
  xxd -i < "$BIN"
  echo "};"
  echo "static const unsigned int g_user_test_vmalloc_size = ${SZ}U;"
} > "$BUILD_DIR/user_test_vmalloc.h"

echo "[+] Building kernel (C/ASM → ELF → BIN)"

# ---- build kernel (the one loaded by kernelLoader) ----
KERNEL_C_OBJS=()
for src in "$KERNEL_DIR"/*.c; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/kernel_$(basename "${src%.c}").o"
  gcc "${CFLAGS[@]}" -I "$BUILD_DIR" -c "$src" -o "$obj"
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
FAT32_LBA=4096       # FAT32 partition starts here (after kernel region)
FAT32_SECTORS=65536  # 32 MiB FAT32 volume

# Total: FAT32 end + small slack
DISK_SECTORS=$((FAT32_LBA + FAT32_SECTORS + 2048))

truncate -s $((DISK_SECTORS * 512)) "$BUILD_DIR/disk.img"

dd if="$BUILD_DIR/boot1.bin"        of="$BUILD_DIR/disk.img" bs=512 seek=0           conv=notrunc status=none
dd if="$BUILD_DIR/boot2.bin"        of="$BUILD_DIR/disk.img" bs=512 seek=1           conv=notrunc status=none
dd if="$BUILD_DIR/kernelLoader.bin" of="$BUILD_DIR/disk.img" bs=512 seek=$LOADER_LBA conv=notrunc status=none
dd if="$BUILD_DIR/kernel.bin"       of="$BUILD_DIR/disk.img" bs=512 seek=$KERNEL_LBA conv=notrunc status=none

# Create FAT32 volume at FAT32_LBA.
# Extract that region, format it, write it back.
FAT32_BYTES=$((FAT32_SECTORS * 512))
FAT32_OFFSET=$((FAT32_LBA * 512))
dd if="$BUILD_DIR/disk.img" of="$BUILD_DIR/fat32.img" bs=512 skip=$FAT32_LBA count=$FAT32_SECTORS status=none
mkfs.fat -F 32 -S 512 "$BUILD_DIR/fat32.img" > /dev/null 2>&1
# Copy any files from build/fs/ into the FAT32 image (if directory exists).
if [ -d "$BUILD_DIR/fs" ]; then
    for f in "$BUILD_DIR/fs"/*; do
        [ -f "$f" ] && mcopy -i "$BUILD_DIR/fat32.img" "$f" :: 2>/dev/null || true
    done
fi
dd if="$BUILD_DIR/fat32.img" of="$BUILD_DIR/disk.img" bs=512 seek=$FAT32_LBA conv=notrunc status=none

stat -c "%n %s" "$BUILD_DIR/disk.img"

echo "[+] Running QEMU"

qemu-system-x86_64 \
  -accel tcg,thread=single \
  -smp 1 \
  -nodefaults \
  -no-user-config \
  -drive format=raw,file=build/disk.img,if=ide \
  -vga std \
  -display sdl \
  -serial stdio \
  -monitor none \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown

