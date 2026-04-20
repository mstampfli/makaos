#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="kernel"
USERLAND_DIR="userland"
BOOT_DIR="boot"
BUILD_DIR="build"
DOOM_DIR="$USERLAND_DIR/apps/doom"

mkdir -p "$BUILD_DIR"

# ── Toolchain ──────────────────────────────────────────────────────────────
CC="gcc"                                      # host gcc: compiles kernel
CLANG="clang"
LD="ld.lld"
NASM="nasm"
OBJCOPY="objcopy"

# Cross-compiler for userland — target-aware, sysroot baked in, knows
# about crt0 + link script + libc without any command-line flag soup.
# Built by scripts/build-toolchain.sh; falls back to host gcc if absent
# (early-development convenience only — production path is the cross).
USER_CC="$(pwd)/toolchain/bin/x86_64-pc-makaos-gcc"
if [ ! -x "$USER_CC" ]; then
  echo "[!] cross-toolchain missing — run scripts/build-toolchain.sh"
  echo "    falling back to host gcc for userland (legacy path)"
  USER_CC="$CC"
fi

# ── Kernel compile flags (clang, ELF64, freestanding) ─────────────────────
KERNEL_INCLUDES=(
  -I "$KERNEL_DIR"
  -I "$KERNEL_DIR/include"
  -I "$KERNEL_DIR/mm"
  -I "$KERNEL_DIR/proc"
  -I "$KERNEL_DIR/fs"
  -I "$KERNEL_DIR/drivers/input"
  -I "$KERNEL_DIR/drivers/video"
  -I "$KERNEL_DIR/drivers/audio"
  -I "$KERNEL_DIR/drivers/storage"
  -I "$KERNEL_DIR/drivers/pci"
  -I "$KERNEL_DIR/drivers/tty"
  -I "$KERNEL_DIR/acpi"
  -I "$KERNEL_DIR/time"
  -I "$KERNEL_DIR/syscall"
  -I "$KERNEL_DIR/security"
  -I "$KERNEL_DIR/net"
  -I "$KERNEL_DIR/io"
  -I "$KERNEL_DIR/crypto"
  -I "$KERNEL_DIR/arch/x86_64"
  -I "$BUILD_DIR"
)

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
  # -O2 for speed.  -g keeps DWARF debug info so GDB still works; the
  # kernel's hot paths (rcu_read_lock, kmalloc, fb_term_scroll, syscall
  # dispatch, memcpy-based copies) depend on the compiler actually
  # inlining and register-allocating.  Hot inlined helpers additionally
  # carry __attribute__((always_inline)) so they inline even at -O0.
  -O2
  -g
  -fno-strict-aliasing          # kernel casts through pointers all the time
  -D__KERNEL__                  # kernel code uses common.h; userland uses libc.h
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

# ── Kernel source files to exclude ────────────────────────────────────────
KERNEL_EXCLUDE="ata_poll.c ata_driver_irq.c fat32.c ac97.c asm_offsets.c"

echo "[+] Building user binaries"

# ── Sysroot (Phase B) ──────────────────────────────────────────────────────
# Every ported library and every future external build targets this tree.
# The only thing the repo owns inside is headers under userland/libc/include
# and the libc .c sources — everything else is rebuilt on every run.
SYSROOT="$BUILD_DIR/sysroot"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib"
cp -rT "$USERLAND_DIR/libc/include" "$SYSROOT/usr/include"

# Shared cross-compile flags any sysroot consumer uses.  -nostdinc forces
# the sysroot include path so host glibc headers never leak in.
SYSROOT_CFLAGS=(
  --sysroot="$SYSROOT"
  -nostdinc
  -isystem "$SYSROOT/usr/include"
)

# ── Common user object files ───────────────────────────────────────────────
"$NASM" -f elf64 "$USERLAND_DIR/entry/entry.asm"    -o "$BUILD_DIR/user_entry.o"
"$NASM" -f elf64 "$USERLAND_DIR/libc/setjmp.asm"    -o "$BUILD_DIR/user_setjmp.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/libc.c"  -o "$BUILD_DIR/user_libc.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/stdio.c" -o "$BUILD_DIR/user_stdio.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/dns.c"   -o "$BUILD_DIR/user_dns.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/libc/math.c" -o "$BUILD_DIR/user_math.o"

# syscalls.c: extern-linkage wrappers around the raw syscall* ops for
# sysroot consumers that can't see libc.h's static-inline copies.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/syscalls.c" -o "$BUILD_DIR/user_syscalls.o"

# libc archive — anything linking sysroot-style pulls this in as -lc.
ar rcs "$SYSROOT/usr/lib/libc.a" \
   "$BUILD_DIR/user_libc.o" "$BUILD_DIR/user_stdio.o" \
   "$BUILD_DIR/user_dns.o" "$BUILD_DIR/user_math.o" \
   "$BUILD_DIR/user_setjmp.o" "$BUILD_DIR/user_syscalls.o"

# crt0 — startup code sysroot-linked binaries get via STARTFILE_SPEC once
# the real cross-gcc is in place.  For the current host-gcc path we still
# list user_entry.o explicitly in each link line.
cp "$BUILD_DIR/user_entry.o" "$SYSROOT/usr/lib/crt0.o"

# Default link script for sysroot users.
cp "$USERLAND_DIR/link.ld" "$SYSROOT/usr/lib/makaos-link.ld"

# ── mbedTLS (userspace port, sysroot-based) ────────────────────────────────
# port-mbedtls.sh fetches the upstream tarball into build/third_party/,
# compiles it against $SYSROOT, and installs libmbedtls.a + headers into
# the sysroot.  Nothing about this port lives inside the MakaOS repo.
MBEDTLS_LIB="$SYSROOT/usr/lib/libmbedtls.a"
if [ ! -f "$MBEDTLS_LIB" ] || [ "${REBUILD_MBEDTLS:-0}" = "1" ]; then
  echo "[+] Building mbedTLS (first-time port can take ~2 minutes)"
  SYSROOT="$SYSROOT" bash scripts/port-mbedtls.sh
fi

# Glue — hardware_poll / ms_time / BIO shims.  Compiles sysroot-style:
# the same include topology as any external client of mbedTLS.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' \
  -I scripts/configs \
  -c scripts/glue/mbedtls_glue.c \
  -o "$BUILD_DIR/user_mbedtls_glue.o"

USER_INCLUDES=(
  -I "$USERLAND_DIR/libc"
  -I "$USERLAND_DIR/include"
)

USER_RT=(
    "$BUILD_DIR/user_entry.o"
    "$BUILD_DIR/user_libc.o"
    "$BUILD_DIR/user_stdio.o"
    "$BUILD_DIR/user_dns.o"
    "$BUILD_DIR/user_math.o"
    "$BUILD_DIR/user_setjmp.o"
)

USER_LINK="$USERLAND_DIR/link.ld"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -I"$USERLAND_DIR/apps/home" -c "$USERLAND_DIR/apps/home/home.c" -o "$BUILD_DIR/user_home.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_home.o" \
   -o "$BUILD_DIR/user_home.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/test_vmalloc/test_vmalloc.c" -o "$BUILD_DIR/user_test_vmalloc.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_test_vmalloc.o" \
   -o "$BUILD_DIR/user_test_vmalloc.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_test_vmalloc.elf" "$BUILD_DIR/user_test_vmalloc.bin"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/hello/hello.c" -o "$BUILD_DIR/user_hello.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_hello.o" \
   -o "$BUILD_DIR/user_hello.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_hello.elf" "$BUILD_DIR/user_hello.bin"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/helloraw/helloraw.c" -o "$BUILD_DIR/user_helloraw.o"
ld -nostdlib -T "$USER_LINK" --entry=_start "$BUILD_DIR/user_helloraw.o" \
   -o "$BUILD_DIR/user_helloraw.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_helloraw.elf" "$BUILD_DIR/user_helloraw.bin"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/test_posix1/test_posix1.c" -o "$BUILD_DIR/user_test_posix1.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_test_posix1.o" \
   -o "$BUILD_DIR/user_test_posix1.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/test_io_uring/test_io_uring.c" -o "$BUILD_DIR/user_test_io_uring.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_test_io_uring.o" \
   -o "$BUILD_DIR/user_test_io_uring.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/smp_test/smp_test.c" -o "$BUILD_DIR/user_smp_test.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_smp_test.o" \
   -o "$BUILD_DIR/user_smp_test.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/apps/tone/tone.c" -o "$BUILD_DIR/user_tone.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_tone.o" \
   -o "$BUILD_DIR/user_tone.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -I"$USERLAND_DIR/apps/shell" -c "$USERLAND_DIR/apps/shell/shell.c" -o "$BUILD_DIR/user_shell.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_shell.o" \
   -o "$BUILD_DIR/user_shell.elf"

# ── Bash 5.2 (optional — requires /tmp/bash-5.2 source tree) ──────────────
BASH_SRC="${BASH_SRC:-/tmp/bash-5.2}"
if [ -d "$BASH_SRC" ]; then
    if [ "$BASH_SRC/builtins/mkbuiltins" -nt "$BUILD_DIR/user_bash.elf" ] || \
       [ ! -f "$BUILD_DIR/user_bash.elf" ]; then
        echo "[+] Building bash 5.2"
        BASH_SRC="$BASH_SRC" "$SCRIPT_DIR/build_bash.sh" "$BASH_SRC" 2>&1 | grep -E "^\[|error:" || true
    else
        echo "[+] bash ELF up to date"
    fi
fi

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/ksec/ksec.c" -o "$BUILD_DIR/user_ksec.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_ksec.o" \
   -o "$BUILD_DIR/user_ksec.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/ps/ps.c" -o "$BUILD_DIR/user_ps.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_ps.o" \
   -o "$BUILD_DIR/user_ps.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/net/net.c" -o "$BUILD_DIR/user_net.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_net.o" \
   -o "$BUILD_DIR/user_net.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/svcmgr/svcmgr.c" -o "$BUILD_DIR/user_svcmgr.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_svcmgr.o" \
   -o "$BUILD_DIR/user_svcmgr.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/restrict/restrict.c" -o "$BUILD_DIR/user_restrict.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_restrict.o" \
   -o "$BUILD_DIR/user_restrict.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" -c "$USERLAND_DIR/apps/http_get/http_get.c" -o "$BUILD_DIR/user_http_get.o"
# https_client.c pulls mbedtls headers from the sysroot — no per-app
# shim dirs, just -isystem $SYSROOT/usr/include.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' \
  -I scripts/configs \
  -c "$USERLAND_DIR/apps/http_get/https_client.c" \
  -o "$BUILD_DIR/user_http_get_tls.o"
"$USER_CC" "${USER_CFLAGS[@]}" \
   "$BUILD_DIR/user_http_get.o" \
   "$BUILD_DIR/user_http_get_tls.o" \
   "$BUILD_DIR/user_mbedtls_glue.o" \
   "$SYSROOT/usr/lib/libmbedtls.a" \
   -o "$BUILD_DIR/user_http_get.elf"


"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/login/login.c" -o "$BUILD_DIR/user_login.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_login.o" \
   -o "$BUILD_DIR/user_login.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -I"$USERLAND_DIR/apps/ls" -c "$USERLAND_DIR/apps/ls/ls.c" -o "$BUILD_DIR/user_ls.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_ls.o" \
   -o "$BUILD_DIR/user_ls.elf"

# ── Coreutils (cat, echo, mkdir, rm, mv, clear, reboot) ─────────────────
for util in cat echo mkdir rm mv clear reboot; do
    "$CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
        -c "$USERLAND_DIR/apps/$util/$util.c" -o "$BUILD_DIR/user_${util}.o"
    ld -nostdlib -T "$USER_LINK" "${USER_RT[@]}" "$BUILD_DIR/user_${util}.o" \
       -o "$BUILD_DIR/user_${util}.elf"
done

# ── Display server (compositor + client library + demo) ────────────────────
DISPLAY_DIR="$USERLAND_DIR/apps/display"
DISPLAY_INCLUDES=("${USER_INCLUDES[@]}" -I"$DISPLAY_DIR")

"$USER_CC" "${USER_CFLAGS[@]}" "${DISPLAY_INCLUDES[@]}" -c "$DISPLAY_DIR/libdisplay.c" -o "$BUILD_DIR/user_libdisplay.o"

"$USER_CC" "${USER_CFLAGS[@]}" "${DISPLAY_INCLUDES[@]}" -c "$DISPLAY_DIR/compositor.c" -o "$BUILD_DIR/user_compositor.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_compositor.o" \
   -o "$BUILD_DIR/user_compositor.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${DISPLAY_INCLUDES[@]}" -c "$DISPLAY_DIR/demo_client.c" -o "$BUILD_DIR/user_demo_client.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_libdisplay.o" "$BUILD_DIR/user_demo_client.o" \
   -o "$BUILD_DIR/user_demo_client.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${DISPLAY_INCLUDES[@]}" -c "$DISPLAY_DIR/terminal.c" -o "$BUILD_DIR/user_terminal.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_libdisplay.o" "$BUILD_DIR/user_terminal.o" \
   -o "$BUILD_DIR/user_terminal.elf"

echo "[+] Building kernel (clang + ld.lld)"

KERNEL_C_OBJS=()

# Collect all .c files from all kernel subdirs (excluding KERNEL_EXCLUDE)
KERNEL_SUBDIRS=(
    "$KERNEL_DIR"
    "$KERNEL_DIR/main"
    "$KERNEL_DIR/mm"
    "$KERNEL_DIR/proc"
    "$KERNEL_DIR/fs"
    "$KERNEL_DIR/drivers/input"
    "$KERNEL_DIR/drivers/video"
    "$KERNEL_DIR/drivers/audio"
    "$KERNEL_DIR/drivers/storage"
    "$KERNEL_DIR/drivers/pci"
    "$KERNEL_DIR/drivers/tty"
    "$KERNEL_DIR/acpi"
    "$KERNEL_DIR/time"
    "$KERNEL_DIR/syscall"
    "$KERNEL_DIR/security"
    "$KERNEL_DIR/arch/x86_64"
    "$KERNEL_DIR/net"
    "$KERNEL_DIR/io"
    "$KERNEL_DIR/crypto"
)

for dir in "${KERNEL_SUBDIRS[@]}"; do
  for src in "$dir"/*.c; do
    [ -e "$src" ] || continue
    base=$(basename "$src")
    if [[ " $KERNEL_EXCLUDE " == *" $base "* ]]; then
      echo "    [skip] $base"
      continue
    fi
    obj="$BUILD_DIR/kernel_${base%.c}.o"
    "$CLANG" "${KERNEL_CFLAGS[@]}" "${KERNEL_INCLUDES[@]}" -c "$src" -o "$obj"
    KERNEL_C_OBJS+=("$obj")
  done
done

# ── asm-offsets: C struct offsets exported to NASM ────────────────────────
# Compile asm_offsets.c to an assembly listing; grep the "->NAME $val NAME"
# marker lines we planted there; rewrite them as NASM `%define`s.  This
# lets syscall_entry.asm reach inside cpu_t without hardcoding offsets.
ASM_OFFSETS_SRC="$KERNEL_DIR/arch/x86_64/asm_offsets.c"
ASM_OFFSETS_S="$BUILD_DIR/asm_offsets.s"
ASM_OFFSETS_INC="$BUILD_DIR/asm_offsets.inc"
"$CLANG" "${KERNEL_CFLAGS[@]}" "${KERNEL_INCLUDES[@]}" -S "$ASM_OFFSETS_SRC" -o "$ASM_OFFSETS_S"
# Planted lines look like:  .ascii "@@@ASMDEF CPU_TSS_RSP0 796"
awk '/@@@ASMDEF/ {
    for (i = 1; i <= NF; i++) if ($i == "\"@@@ASMDEF") { name=$(i+1); val=$(i+2); gsub(/"/,"",val); printf "%%define %s %s\n", name, val; next }
}' "$ASM_OFFSETS_S" > "$ASM_OFFSETS_INC"

KERNEL_ASM_OBJS=()
# The AP trampoline is a flat binary blob — NOT an ELF64 relocatable like
# everything else in arch/x86_64.  It is assembled to raw bytes and then
# packaged into a .o via objcopy so the linker can place it alongside
# kernel code and expose _binary_ap_trampoline_bin_{start,end} symbols
# that smp_boot.c references to copy the blob into its low-memory page.
AP_TRAMP_ASM="$KERNEL_DIR/arch/x86_64/ap_trampoline.asm"
AP_TRAMP_OBJ="$BUILD_DIR/kernel_ap_trampoline.o"
"$NASM" -f bin "$AP_TRAMP_ASM" -o "$BUILD_DIR/ap_trampoline.bin"
# objcopy synthesises three absolute symbols from the binary input, but
# it bakes the *input path* into the symbol names (dots and slashes →
# underscores).  Run objcopy from inside BUILD_DIR so the symbols come
# out as _binary_ap_trampoline_bin_{start,end,size} — the exact names
# smp_boot.c declares extern.
#
# --rename-section parks the bytes in .rodata so they live inside the
# kernel image (same region the final kernel.bin objcopy grabs via
# -j .rodata).
( cd "$BUILD_DIR" && "$OBJCOPY" -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
    ap_trampoline.bin "$(basename "$AP_TRAMP_OBJ")" )
KERNEL_ASM_OBJS+=("$AP_TRAMP_OBJ")

for src in "$KERNEL_DIR/arch/x86_64"/*.asm; do
  [ -e "$src" ] || continue
  # Skip the trampoline — it has its own flat-binary build step above.
  if [ "$src" = "$AP_TRAMP_ASM" ]; then continue; fi
  obj="$BUILD_DIR/kernel_$(basename "${src%.asm}").o"
  "$NASM" -f elf64 -g -F dwarf -I "$BUILD_DIR/" "$src" -o "$obj"
  KERNEL_ASM_OBJS+=("$obj")
done

"$LD" -nostdlib --no-dynamic-linker \
  -z max-page-size=0x1000 \
  -T "$KERNEL_DIR/link.ld" \
  "${KERNEL_ASM_OBJS[@]}" "${KERNEL_C_OBJS[@]}" \
  -o "$BUILD_DIR/kernel.elf"

"$OBJCOPY" -O binary \
  -j .text -j .rodata -j .initcall_0 -j .initcall_1 -j .data -j .bss \
  "$BUILD_DIR/kernel.elf" "$BUILD_DIR/kernel.bin"

echo "[+] Building UEFI bootloader (clang → PE32+)"

"$CLANG" "${EFI_CFLAGS[@]}" \
  -I "$BOOT_DIR/uefi" \
  -c "$BOOT_DIR/uefi/main.c" \
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
ESP_START=2048
ESP_END=4095

DISK_SECTORS=$((EXT2_LBA + EXT2_SECTORS + 2048))
truncate -s $((DISK_SECTORS * 512)) "$BUILD_DIR/disk.img"

sgdisk --clear \
  --new=1:${ESP_START}:${ESP_END} \
  --typecode=1:ef00 \
  --change-name=1:"EFI System" \
  "$BUILD_DIR/disk.img" > /dev/null 2>&1

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

# Helper: set inode mode + uid + gid via debugfs.
# Usage: ext2_chmod <img> <path> <octal_mode_with_type> <uid> <gid>
# mode_with_type example: 0100755 (regular file, rwxr-xr-x)
#                         0040755 (directory,    rwxr-xr-x)
#                         0100644 (regular file, rw-r--r--)
ext2_setperm() {
    local img="$1" path="$2" mode="$3" uid="$4" gid="$5"
    debugfs -w "$img" -R "set_inode_field $path mode $mode"    > /dev/null 2>&1 || true
    debugfs -w "$img" -R "set_inode_field $path uid $uid"      > /dev/null 2>&1 || true
    debugfs -w "$img" -R "set_inode_field $path gid $gid"      > /dev/null 2>&1 || true
}

# Create directory tree.
# ── User database files ───────────────────────────────────────────────────
# Generate /etc/passwd and /etc/shadow into a temp staging dir.
mkdir -p "$BUILD_DIR/etc_stage"

cat > "$BUILD_DIR/etc_stage/passwd" <<'PASSWD_EOF'
root:0:0:/root:/bin/bash
maka:1000:1000:/home/maka:/bin/bash
PASSWD_EOF

# /etc/shadow — plaintext passwords for now (no crypto).
# root password: "toor", maka password: "maka"
cat > "$BUILD_DIR/etc_stage/shadow" <<'SHADOW_EOF'
root:toor
maka:maka
SHADOW_EOF

# Directories: root-owned, 0755 (rwxr-xr-x) except /root (0700) and /tmp (1777).
debugfs -w "$BUILD_DIR/ext2.img" <<'DEBUGFS_EOF' > /dev/null 2>&1
mkdir bin
mkdir etc
mkdir etc/services
mkdir home
mkdir tmp
mkdir dev
mkdir root
DEBUGFS_EOF

# Set directory permissions (type bits: 0040000 = directory).
ext2_setperm "$BUILD_DIR/ext2.img" /         0040755 0 0      # / root:root rwxr-xr-x
ext2_setperm "$BUILD_DIR/ext2.img" /bin      0040755 0 0      # /bin root:root rwxr-xr-x
ext2_setperm "$BUILD_DIR/ext2.img" /etc      0040755 0 0      # /etc root:root rwxr-xr-x
ext2_setperm "$BUILD_DIR/ext2.img" /home     0040755 0 0      # /home root:root rwxr-xr-x
ext2_setperm "$BUILD_DIR/ext2.img" /tmp      0041777 0 0      # /tmp root:root rwxrwxrwx + sticky
ext2_setperm "$BUILD_DIR/ext2.img" /dev      0040755 0 0      # /dev root:root rwxr-xr-x
ext2_setperm "$BUILD_DIR/ext2.img" /root     0040700 0 0      # /root root:root rwx------

# /home/maka — uid=1000 gid=1000, 0700 (private home)
debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir home/maka" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /home/maka 0040700 1000 1000

# Install /etc/passwd (root:root 0644 — world-readable, no sensitive data)
debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/passwd etc/passwd" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /etc/passwd 0100644 0 0

# Install /etc/shadow (root:root 0600 — root-only, contains passwords)
debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/shadow etc/shadow" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /etc/shadow 0100600 0 0

# Put a secret file in /root to test permission enforcement
echo "root's secret — maka cannot read this" > "$BUILD_DIR/etc_stage/root_secret.txt"
debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/root_secret.txt root/secret.txt" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /root/secret.txt 0100600 0 0

echo "[+] /etc/passwd and /etc/shadow installed"

# ── Service definitions ───────────────────────────────────────────────────
mkdir -p "$BUILD_DIR/etc_stage/services"

cat > "$BUILD_DIR/etc_stage/services/net.svc" <<'SVC_EOF'
path=/bin/net
uid=0
gid=0
stdio=null
pledge=net stdio cpath wpath
unveil=/etc rwc
restart=on-failure
SVC_EOF

debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/services/net.svc etc/services/net.svc" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /etc/services/net.svc 0100644 0 0
ext2_setperm "$BUILD_DIR/ext2.img" /etc/services         0040755 0 0
ext2_setperm "$BUILD_DIR/ext2.img" /etc                  0040755 0 0
echo "[+] /etc/services/net.svc installed"

# Install binaries with execute permissions (0755 = rwxr-xr-x, type 0100000 = regular file).
# All binaries: root:root 0755 — any user can execute, only root can write.
ext2_install_bin() {
    local img="$1" src="$2" dst="$3"
    debugfs -w "$img" -R "write $src $dst" > /dev/null 2>&1 || true
    ext2_setperm "$img" "/$dst" 0100755 0 0
}

if [ -f "$BUILD_DIR/user_test_vmalloc.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_test_vmalloc.elf" bin/vmalloc
fi
if [ -f "$BUILD_DIR/user_home.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_home.elf" bin/home
fi
if [ -f "$BUILD_DIR/user_hello.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_hello.elf" bin/hello
fi
if [ -f "$BUILD_DIR/user_helloraw.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_helloraw.elf" bin/helloraw
fi
if [ -f "$BUILD_DIR/user_test_posix1.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_test_posix1.elf" bin/test_posix1
fi
if [ -f "$BUILD_DIR/user_test_io_uring.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_test_io_uring.elf" bin/test_io_uring
fi
if [ -f "$BUILD_DIR/user_smp_test.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_smp_test.elf" bin/smp_test
    echo "[+] smp_test ELF installed at bin/smp_test (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_tone.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_tone.elf" bin/tone
fi
if [ -f "$BUILD_DIR/user_shell.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_shell.elf" bin/shell
    echo "[+] shell ELF installed at bin/shell (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_ps.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_ps.elf" bin/ps
    echo "[+] ps ELF installed at bin/ps (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_net.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_net.elf" bin/net
    echo "[+] net ELF installed at bin/net (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_svcmgr.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_svcmgr.elf" bin/svcmgr
    echo "[+] svcmgr ELF installed at bin/svcmgr (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_restrict.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_restrict.elf" bin/restrict
    echo "[+] restrict ELF installed at bin/restrict (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_http_get.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_http_get.elf" bin/http_get
    echo "[+] http_get ELF installed at bin/http_get (root:root 0755)"
fi

if [ -f "$BUILD_DIR/user_ksec.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_ksec.elf" bin/ksec
    echo "[+] ksec ELF installed at bin/ksec (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_login.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_login.elf" bin/login
    echo "[+] login ELF installed at bin/login (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_ls.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_ls.elf" bin/ls
    echo "[+] ls ELF installed at bin/ls (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_doom.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_doom.elf" bin/doom
    echo "[+] doom ELF installed at bin/doom (root:root 0755)"
fi

if [ -f "$BUILD_DIR/user_bash.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_bash.elf" bin/bash
    echo "[+] bash ELF installed at bin/bash (root:root 0755)"
fi

# ── Coreutils ────────────────────────────────────────────────────────────
for util in cat echo mkdir rm mv clear reboot; do
    if [ -f "$BUILD_DIR/user_${util}.elf" ]; then
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_${util}.elf" "bin/$util"
        echo "[+] $util ELF installed at bin/$util (root:root 0755)"
    fi
done

# ── Display server ────────────────────────────────────────────────────────
if [ -f "$BUILD_DIR/user_compositor.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_compositor.elf" bin/makadisplay
    echo "[+] makadisplay ELF installed at bin/makadisplay (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_demo_client.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_demo_client.elf" bin/demo_client
    echo "[+] demo_client ELF installed at bin/demo_client (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_terminal.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_terminal.elf" bin/makaterm
    echo "[+] makaterm ELF installed at bin/makaterm (root:root 0755)"
fi

WAD_SEARCH=(
    "$DOOM_DIR/doom1.wad"
    "$DOOM_DIR/DOOM1.WAD"
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
    # WAD is a data file: root:root 0644 (readable by all, no exec needed)
    ext2_setperm "$BUILD_DIR/ext2.img" /bin/doom1.wad 0100644 0 0
    echo "[+] WAD installed: $WAD_FILE → /bin/doom1.wad (root:root 0644)"
else
    echo "[!] doom1.wad not found — place it at $DOOM_DIR/doom1.wad before running"
fi

if [ -d "$BUILD_DIR/fs" ]; then
    for f in "$BUILD_DIR/fs"/*; do
        [ -f "$f" ] && debugfs -w "$BUILD_DIR/ext2.img" -R "write $f $(basename $f)" > /dev/null 2>&1 || true
    done
fi

dd if="$BUILD_DIR/ext2.img" of="$BUILD_DIR/disk.img" bs=512 seek=$EXT2_LBA conv=notrunc status=none

stat -c "%n %s" "$BUILD_DIR/disk.img"

if [ -n "$NO_QEMU" ]; then
  echo "[+] Build complete — NO_QEMU set, skipping QEMU launch"
  exit 0
fi

echo "[+] Running QEMU (UEFI/OVMF)"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"

cp "$OVMF_VARS_SRC" "$OVMF_VARS"

# ── Free up any port lingering from a prior aborted run ──────────────────
# build.sh is meant to be re-runnable; a previous QEMU instance still
# holding the host-port forward (or the gdbstub on :1234) would block
# the new bind.  pkill is targeted by command line so we don't touch
# unrelated qemus the user might have running.
pkill -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true
for _i in 1 2 3 4 5; do
    if ! ss -tln 2>/dev/null | grep -qE ":(18080|1234)\b"; then break; fi
    sleep 0.2
done

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
  -vga std \
  -display none \
  -audiodev pa,id=snd0 \
  -device intel-hda \
  -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0,hostfwd=tcp::18080-:80 \
  -device virtio-net-pci,netdev=net0 \
  -object filter-dump,id=f1,netdev=net0,file=build/net.pcap \
  -serial file:build/serial.txt \
  -monitor none \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown
