#!/usr/bin/env bash
set -e

KERNEL_DIR="kernel"
USER_DIR="user"
UEFI_DIR="uefi/boot"
BUILD_DIR="build"

mkdir -p "$BUILD_DIR"

# ── Toolchain ──────────────────────────────────────────────────────────────
CC="gcc"
CLANG="clang"
LD="ld.lld"
NASM="nasm"
OBJCOPY="objcopy"

# ── Kernel compile flags (clang, ELF64, freestanding) ─────────────────────
KERNEL_CFLAGS=(
  -ffreestanding
  -m64
  -mno-red-zone
  -mgeneral-regs-only
  -mno-sse -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4 -mno-sse4.1 -mno-sse4.2
  -mno-avx -mno-avx2
  -mno-mmx
  -fno-pie -fno-pic -fno-plt
  -fno-stack-protector
  -fno-builtin
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-omit-frame-pointer
  -mcmodel=kernel
  -O0
  -g
  -Wall -Wextra
  -Wno-unused-parameter
  -Wno-missing-field-initializers
)

# ── User compile flags ─────────────────────────────────────────────────────
USER_CFLAGS=(
  -ffreestanding -m64 -mno-red-zone
  -fno-pie -fno-pic -fno-plt
  -fno-stack-protector -fno-builtin
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -O0 -Wall -Wextra
  -Wno-unused-parameter
)

# ── UEFI compile flags (clang → PE32+, MS ABI, freestanding) ──────────────
EFI_CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding
  -fno-stack-protector
  -fno-builtin
  -fshort-wchar
  -mno-red-zone
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-exceptions
  -O2
)

# ── ATA drivers to exclude from kernel build ───────────────────────────────
KERNEL_EXCLUDE="ata_poll.c ata_driver_irq.c fat32.c"

echo "[+] Building user binaries"

# ── Common user object files ───────────────────────────────────────────────
"$NASM" -f elf64 "$USER_DIR/entry.asm"   -o "$BUILD_DIR/user_entry.o"
"$NASM" -f elf64 "$USER_DIR/setjmp.asm"  -o "$BUILD_DIR/user_setjmp.o"
"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/libc.c"  -o "$BUILD_DIR/user_libc.o"
"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/stdio.c" -o "$BUILD_DIR/user_stdio.o"
"$CC" "${USER_CFLAGS[@]}" -msse2 -c "$USER_DIR/math.c" -o "$BUILD_DIR/user_math.o"

# Base runtime objects linked into every user binary that uses libc.
USER_RT=(
    "$BUILD_DIR/user_entry.o"
    "$BUILD_DIR/user_libc.o"
    "$BUILD_DIR/user_stdio.o"
    "$BUILD_DIR/user_math.o"
    "$BUILD_DIR/user_setjmp.o"
)

"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/test_vmalloc.c" -o "$BUILD_DIR/user_test_vmalloc.o"
ld -nostdlib -T "$USER_DIR/user_link.ld" "${USER_RT[@]}" "$BUILD_DIR/user_test_vmalloc.o" \
   -o "$BUILD_DIR/user_test_vmalloc.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_test_vmalloc.elf" "$BUILD_DIR/user_test_vmalloc.bin"

"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/hello.c" -o "$BUILD_DIR/user_hello.o"
ld -nostdlib -T "$USER_DIR/user_link.ld" "${USER_RT[@]}" "$BUILD_DIR/user_hello.o" \
   -o "$BUILD_DIR/user_hello.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_hello.elf" "$BUILD_DIR/user_hello.bin"

"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/helloraw.c" -o "$BUILD_DIR/user_helloraw.o"
ld -nostdlib -T "$USER_DIR/user_link.ld" "$BUILD_DIR/user_entry.o" "$BUILD_DIR/user_helloraw.o" \
   -o "$BUILD_DIR/user_helloraw.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_helloraw.elf" "$BUILD_DIR/user_helloraw.bin"

"$CC" "${USER_CFLAGS[@]}" -c "$USER_DIR/test_posix1.c" -o "$BUILD_DIR/user_test_posix1.o"
ld -nostdlib -T "$USER_DIR/user_link.ld" "${USER_RT[@]}" "$BUILD_DIR/user_test_posix1.o" \
   -o "$BUILD_DIR/user_test_posix1.elf"

# ── Doom (doomgeneric) ────────────────────────────────────────────────────
DOOM_DIR="$USER_DIR/doom"
DOOMGENERIC_DIR="$DOOM_DIR/doomgeneric"

if [ ! -d "$DOOMGENERIC_DIR" ]; then
    echo "[+] Cloning doomgeneric..."
    git clone --depth=1 -c http.sslVerify=false \
        https://github.com/ozkl/doomgeneric.git "$DOOMGENERIC_DIR" || \
        echo "[!] git clone failed — place doomgeneric at $DOOMGENERIC_DIR manually"
fi

if [ -d "$DOOMGENERIC_DIR" ]; then
    echo "[+] Building doom"

    DOOM_CFLAGS=(
        -ffreestanding -m64 -mno-red-zone -msse2
        -fno-pie -fno-pic -fno-plt
        -fno-stack-protector -fno-builtin
        -fno-asynchronous-unwind-tables -fno-unwind-tables
        -O1 -g
        # Include search order: our shim headers first, then doomgeneric internals.
        -nostdinc
        -I"$DOOM_DIR/include"
        -I"$DOOMGENERIC_DIR/doomgeneric"
        -I"$USER_DIR"
        # Suppress warnings from doom source we can't modify.
        -Wno-implicit-function-declaration
        -Wno-implicit-int
        -Wno-int-conversion
        -Wno-pointer-sign
        -Wno-missing-field-initializers
        -Wno-char-subscripts
        -Wno-sign-compare
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-parameter
    )

    DOOM_SRCS=()
    DOOM_OBJS=()

    # Compile doomgeneric core sources (everything in doomgeneric/ except the
    # platform-specific file doomgeneric_*.c which we replace with our own).
    for src in "$DOOMGENERIC_DIR/doomgeneric/"*.c; do
        base=$(basename "$src")
        # Skip all platform-specific and audio/sound files:
        # - doomgeneric_*.c: platform backends (SDL, xlib, allegro, etc.)
        # - i_sound.c, i_sdlmusic.c, i_sdlsound.c: SDL audio (no audio on MakaOS)
        # - i_allegromusic.c, i_allegrosound.c: Allegro audio
        # - i_cdmus.c: CD audio
        case "$base" in
            doomgeneric_*.c) continue ;;
            i_sound.c|i_sdlmusic.c|i_sdlsound.c) continue ;;
            i_allegromusic.c|i_allegrosound.c|i_cdmus.c) continue ;;
        esac
        obj="$BUILD_DIR/doom_${base%.c}.o"
        "$CC" "${DOOM_CFLAGS[@]}" -c "$src" -o "$obj"
        DOOM_OBJS+=("$obj")
    done

    # Compile our platform glue and audio stubs.
    "$CC" "${DOOM_CFLAGS[@]}" -c "$DOOM_DIR/doomgeneric_makaos.c" -o "$BUILD_DIR/doom_makaos.o"
    DOOM_OBJS+=("$BUILD_DIR/doom_makaos.o")
    "$CC" "${DOOM_CFLAGS[@]}" -c "$DOOM_DIR/i_sound_stub.c" -o "$BUILD_DIR/doom_i_sound_stub.o"
    DOOM_OBJS+=("$BUILD_DIR/doom_i_sound_stub.o")

    # Link doom ELF.
    ld -nostdlib -T "$USER_DIR/user_link.ld" \
       "${USER_RT[@]}" \
       "${DOOM_OBJS[@]}" \
       -o "$BUILD_DIR/user_doom.elf"

    echo "[+] Doom ELF built: $(stat -c '%s' "$BUILD_DIR/user_doom.elf") bytes"
else
    echo "[!] doomgeneric not found — skipping doom build"
fi

# Embed user binaries as kernel-side byte arrays
BIN_HELLO="$BUILD_DIR/user_hello.bin"
SZ_HELLO=$(stat -c "%s" "$BIN_HELLO")
{
  echo "#pragma once"
  echo "static const unsigned char g_user_hello[] = {"
  xxd -i < "$BIN_HELLO"
  echo "};"
  echo "static const unsigned int g_user_hello_size = ${SZ_HELLO}U;"
} > "$BUILD_DIR/user_hello.h"

BIN="$BUILD_DIR/user_test_vmalloc.bin"
SZ=$(stat -c "%s" "$BIN")
{
  echo "#pragma once"
  echo "static const unsigned char g_user_test_vmalloc[] = {"
  xxd -i < "$BIN"
  echo "};"
  echo "static const unsigned int g_user_test_vmalloc_size = ${SZ}U;"
} > "$BUILD_DIR/user_test_vmalloc.h"

echo "[+] Building kernel (clang + ld.lld)"

KERNEL_C_OBJS=()
for src in "$KERNEL_DIR"/*.c; do
  [ -e "$src" ] || continue
  base=$(basename "$src")
  # Skip ATA drivers
  if [[ " $KERNEL_EXCLUDE " == *" $base "* ]]; then
    echo "    [skip] $base"
    continue
  fi
  obj="$BUILD_DIR/kernel_${base%.c}.o"
  "$CLANG" "${KERNEL_CFLAGS[@]}" -I "$BUILD_DIR" -I "$KERNEL_DIR" -c "$src" -o "$obj"
  KERNEL_C_OBJS+=("$obj")
done

KERNEL_ASM_OBJS=()
for src in "$KERNEL_DIR"/*.asm; do
  [ -e "$src" ] || continue
  obj="$BUILD_DIR/kernel_$(basename "${src%.asm}").o"
  "$NASM" -f elf64 -g -F dwarf "$src" -o "$obj"
  KERNEL_ASM_OBJS+=("$obj")
done

"$LD" -nostdlib --no-dynamic-linker \
  -z max-page-size=0x1000 \
  -T "$KERNEL_DIR/link.ld" \
  "${KERNEL_ASM_OBJS[@]}" "${KERNEL_C_OBJS[@]}" \
  -o "$BUILD_DIR/kernel.elf"

"$OBJCOPY" -O binary \
  -j .text -j .rodata -j .data -j .bss \
  "$BUILD_DIR/kernel.elf" "$BUILD_DIR/kernel.bin"

echo "[+] Building UEFI bootloader (clang → PE32+)"

"$CLANG" "${EFI_CFLAGS[@]}" \
  -I "$UEFI_DIR" \
  -c "$UEFI_DIR/main.c" \
  -o "$BUILD_DIR/uefi_main.o"

lld-link /nodefaultlib \
  /subsystem:efi_application \
  /entry:efi_main \
  /out:"$BUILD_DIR/BOOTX64.EFI" \
  "$BUILD_DIR/uefi_main.o"

echo "[+] Checking sizes"
stat -c "%n %s" \
  "$BUILD_DIR/kernel.bin" \
  "$BUILD_DIR/BOOTX64.EFI"

echo "[+] Creating disk image (GPT + ESP + ext2)"

EXT2_LBA=4096
EXT2_SECTORS=65536   # 32 MiB
ESP_START=2048       # LBA 2048 (1 MiB aligned)
ESP_END=4095         # LBA 4095 (just before ext2)

DISK_SECTORS=$((EXT2_LBA + EXT2_SECTORS + 2048))
truncate -s $((DISK_SECTORS * 512)) "$BUILD_DIR/disk.img"

# Create GPT with one ESP partition (LBA 2048–4095)
sgdisk --clear \
  --new=1:${ESP_START}:${ESP_END} \
  --typecode=1:ef00 \
  --change-name=1:"EFI System" \
  "$BUILD_DIR/disk.img" > /dev/null 2>&1

# Format ESP as FAT32 using mtools (offset = ESP_START * 512 bytes)
ESP_OFFSET=$((ESP_START * 512))
mformat -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} -F -v "MAKAOS_ESP" ::
mmd -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} ::/EFI
mmd -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} ::/EFI/BOOT
mmd -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} ::/EFI/MAKA
mcopy -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} "$BUILD_DIR/kernel.bin"  ::/EFI/MAKA/kernel.bin

echo "[+] Building ext2 filesystem"

dd if=/dev/zero of="$BUILD_DIR/ext2.img" bs=512 count=$EXT2_SECTORS status=none
mkfs.ext2 -b 1024 -L "MakaOS" "$BUILD_DIR/ext2.img" > /dev/null 2>&1

debugfs -w "$BUILD_DIR/ext2.img" <<'DEBUGFS_EOF' > /dev/null 2>&1
mkdir bin
mkdir etc
mkdir home
mkdir tmp
mkdir dev
DEBUGFS_EOF

if [ -f "$BUILD_DIR/user_test_vmalloc.elf" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/user_test_vmalloc.elf bin/vmalloc" > /dev/null 2>&1 || true
fi
if [ -f "$BUILD_DIR/user_hello.elf" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/user_hello.elf bin/hello" > /dev/null 2>&1 || true
fi
if [ -f "$BUILD_DIR/user_helloraw.elf" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/user_helloraw.elf bin/helloraw" > /dev/null 2>&1 || true
fi
if [ -f "$BUILD_DIR/user_test_posix1.elf" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/user_test_posix1.elf bin/test_posix1" > /dev/null 2>&1 || true
fi
if [ -f "$BUILD_DIR/user_doom.elf" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/user_doom.elf bin/doom" > /dev/null 2>&1 || true
    echo "[+] doom ELF installed at bin/doom"
fi

# Install doom1.wad if present (place next to the binary, or in /home).
# The doom binary opens "doom1.wad" relative to cwd, which the shell sets to
# the directory of the executable (/bin).  So we install it as /bin/doom1.wad.
WAD_SEARCH=(
    "$USER_DIR/doom/doom1.wad"
    "$USER_DIR/doom/DOOM1.WAD"
    "doom1.wad"
    "DOOM1.WAD"
    "$HOME/doom1.wad"
    "$HOME/DOOM1.WAD"
)
WAD_FILE=""
for w in "${WAD_SEARCH[@]}"; do
    if [ -f "$w" ]; then WAD_FILE="$w"; break; fi
done
if [ -n "$WAD_FILE" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $WAD_FILE bin/doom1.wad" > /dev/null 2>&1 || true
    echo "[+] WAD installed: $WAD_FILE → /bin/doom1.wad"
else
    echo "[!] doom1.wad not found — place it at user/doom/doom1.wad before running"
fi

if [ -d "$BUILD_DIR/fs" ]; then
    for f in "$BUILD_DIR/fs"/*; do
        [ -f "$f" ] && debugfs -w "$BUILD_DIR/ext2.img" -R "write $f $(basename $f)" > /dev/null 2>&1 || true
    done
fi

# Write ext2 at LBA 4096 (after ESP partition)
dd if="$BUILD_DIR/ext2.img" of="$BUILD_DIR/disk.img" bs=512 seek=$EXT2_LBA conv=notrunc status=none

stat -c "%n %s" "$BUILD_DIR/disk.img"

echo "[+] Running QEMU (UEFI/OVMF)"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"

# Copy VARS so OVMF can persist NVRAM across runs
cp "$OVMF_VARS_SRC" "$OVMF_VARS"

qemu-system-x86_64 \
  -accel tcg,thread=single \
  -smp 1 \
  -m 256M \
  -nodefaults \
  -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file="$BUILD_DIR/disk.img",if=none,id=hd0 \
  -device ahci,id=ahci \
  -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std \
  -display sdl \
  -serial file:build/serial.txt \
  -monitor none \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown
