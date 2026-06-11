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
  -I "$KERNEL_DIR/debug"
  -I "$KERNEL_DIR/proc"
  -I "$KERNEL_DIR/time"
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
# Do NOT wipe the sysroot here — ports install their own .a + headers
# into it (fontconfig, wayland, freetype, …) and a wipe would destroy
# them on every libc rebuild.  `cp -rT` below overwrites libc headers
# in place, which is all we need for a libc-only refresh.
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
"$NASM" -f elf64 "$USERLAND_DIR/libc/pthread_trampoline.asm" -o "$BUILD_DIR/user_pthread_tramp.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/libc.c"  -o "$BUILD_DIR/user_libc.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/stdio.c" -o "$BUILD_DIR/user_stdio.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/dns.c"   -o "$BUILD_DIR/user_dns.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/libc/math.c" -o "$BUILD_DIR/user_math.o"

# POSIX-header-aligned syscall wrapper files.  One file per header —
# each provides link-resolvable extern symbols for sysroot consumers
# (ports linked via -lc).  libc.h keeps its static-inline copies for
# in-tree apps that include it directly.
for src in unistd fcntl sys_stat sys_socket sys_eventfd sys_timerfd \
           sys_signalfd sys_ioctl sys_time sys_file sys_epoll \
           time arpa_inet string ctype makaos_input poll signal resolv \
           getopt wordexp; do
  "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
    -c "$USERLAND_DIR/libc/${src}.c" -o "$BUILD_DIR/user_${src}.o"
done

# pthread: POSIX threading on top of sys_thread + sched_yield.  Spins
# on contention until we add a kernel futex.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/pthread.c" -o "$BUILD_DIR/user_pthread.o"

# Split POSIX-header files for sysroot consumers.  One-file-per-header
# clean layout: dlfcn.c, locale.c, langinfo.c, assert.c, stdlib.c.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/dlfcn.c"    -o "$BUILD_DIR/user_dlfcn.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/locale.c"   -o "$BUILD_DIR/user_locale.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/langinfo.c" -o "$BUILD_DIR/user_langinfo.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/assert.c"   -o "$BUILD_DIR/user_assert.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/stdlib.c"   -o "$BUILD_DIR/user_stdlib.o"

# wayland-egl stubs — Mesa's wayland-egl isn't ported yet, but SDL3's
# Wayland dyn-loader shim references the symbols unconditionally.
# Stub them so static link resolves; never invoked on the software
# path (guarded by SDL_VIDEO_OPENGL_EGL=0 at configure time).
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/wayland_egl_stub.c" -o "$BUILD_DIR/user_wayland_egl_stub.o"

# Extern impls for symbols only exposed inline in the in-tree libc.h
# (wcslen/wcsncmp/wcscmp, iconv family, _Exit) — link-visible for
# sysroot consumers like SDL3.  Compiled with SYSROOT_CFLAGS so its
# wchar.h / stddef.h types agree with its callers.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/sdl_port_stubs.c" -o "$BUILD_DIR/user_sdl_port_stubs.o"

# C11 <threads.h> shim + syslog client stub.  fcft (-> foot) and a
# handful of other ports use C11 threads even when pthread is
# available.  Syslog piggy-backs in the same TU because both are
# small and both forward to lower-level primitives we already have.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/c11_threads.c" -o "$BUILD_DIR/user_c11_threads.o"

# POSIX unnamed semaphores — spin-and-yield implementation.  foot's
# render pump uses a single sem_t; contention is rare.  Replace with
# a futex-backed version when the kernel gains one.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/semaphore.c" -o "$BUILD_DIR/user_semaphore.o"

# <wctype.h> — ASCII-only wide-char classification.  Full Unicode
# handling routes through utf8proc where it matters.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
  -c "$USERLAND_DIR/libc/wctype.c" -o "$BUILD_DIR/user_wctype.o"

# libc archive — anything linking sysroot-style pulls this in as -lc.
ar rcs "$SYSROOT/usr/lib/libc.a" \
   "$BUILD_DIR/user_libc.o"    "$BUILD_DIR/user_stdio.o" \
   "$BUILD_DIR/user_dns.o"     "$BUILD_DIR/user_math.o" \
   "$BUILD_DIR/user_setjmp.o"  "$BUILD_DIR/user_pthread.o" \
   "$BUILD_DIR/user_pthread_tramp.o" \
   "$BUILD_DIR/user_dlfcn.o"    "$BUILD_DIR/user_locale.o" \
   "$BUILD_DIR/user_langinfo.o" "$BUILD_DIR/user_assert.o" \
   "$BUILD_DIR/user_stdlib.o"   "$BUILD_DIR/user_string.o" \
   "$BUILD_DIR/user_unistd.o"   "$BUILD_DIR/user_fcntl.o" \
   "$BUILD_DIR/user_sys_stat.o" "$BUILD_DIR/user_sys_socket.o" \
   "$BUILD_DIR/user_sys_eventfd.o"  "$BUILD_DIR/user_sys_timerfd.o" \
   "$BUILD_DIR/user_sys_signalfd.o" "$BUILD_DIR/user_sys_ioctl.o" \
   "$BUILD_DIR/user_sys_time.o" "$BUILD_DIR/user_sys_file.o" \
   "$BUILD_DIR/user_sys_epoll.o" \
   "$BUILD_DIR/user_time.o" "$BUILD_DIR/user_arpa_inet.o" \
   "$BUILD_DIR/user_ctype.o" "$BUILD_DIR/user_makaos_input.o" \
   "$BUILD_DIR/user_poll.o" "$BUILD_DIR/user_signal.o" \
   "$BUILD_DIR/user_resolv.o" "$BUILD_DIR/user_getopt.o" \
   "$BUILD_DIR/user_wordexp.o" \
   "$BUILD_DIR/user_wayland_egl_stub.o" \
   "$BUILD_DIR/user_sdl_port_stubs.o" \
   "$BUILD_DIR/user_c11_threads.o" \
   "$BUILD_DIR/user_semaphore.o" \
   "$BUILD_DIR/user_wctype.o"

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

# ── Tier 2 foundation libraries (fast idempotent ports) ────────────────
# Each port script is a no-op when its target sits newer than its
# sources, so re-running build.sh is cheap.  Sysroot is always self-
# consistent because build.sh wipes + re-populates it every run.
for port in zlib expat libffi pixman libxkbcommon freetype harfbuzz libdrm; do
  echo "[+] Building Tier 2 lib: $port"
  SYSROOT="$SYSROOT" bash "scripts/port-$port.sh"
done

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
    # POSIX-header-split extern wrappers — needed for in-tree apps now
    # that libc.h declares these as prototypes rather than inlining them.
    "$BUILD_DIR/user_unistd.o"
    "$BUILD_DIR/user_fcntl.o"
    "$BUILD_DIR/user_sys_stat.o"
    "$BUILD_DIR/user_sys_socket.o"
    "$BUILD_DIR/user_sys_eventfd.o"
    "$BUILD_DIR/user_sys_timerfd.o"
    "$BUILD_DIR/user_sys_signalfd.o"
    "$BUILD_DIR/user_sys_ioctl.o"
    "$BUILD_DIR/user_sys_time.o"
    "$BUILD_DIR/user_sys_file.o"
    "$BUILD_DIR/user_sys_epoll.o"
    "$BUILD_DIR/user_time.o"
    "$BUILD_DIR/user_ctype.o"
    "$BUILD_DIR/user_makaos_input.o"
    "$BUILD_DIR/user_poll.o"
    "$BUILD_DIR/user_signal.o"
    "$BUILD_DIR/user_arpa_inet.o"
    "$BUILD_DIR/user_string.o"
    "$BUILD_DIR/user_stdlib.o"
    "$BUILD_DIR/user_dlfcn.o"
    "$BUILD_DIR/user_locale.o"
    "$BUILD_DIR/user_langinfo.o"
    "$BUILD_DIR/user_assert.o"
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

# ── sdl3_hello — end-to-end SDL3 smoke test ──────────────────────────
# Links against the sysroot-installed libSDL3.a + its Wayland/xkbcommon
# deps + libc/libm/libpthread.  Uses -nostartfiles + crt0.o like every
# other sysroot-linked app so argv/argc are populated correctly.
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ]; then
    "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
        -I "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/sdl3_hello/sdl3_hello.c" \
        -o "$BUILD_DIR/user_sdl3_hello.o"
    "$USER_CC" "${USER_CFLAGS[@]}" --sysroot="$SYSROOT" \
        -nostartfiles -Wl,--build-id=none \
        "$SYSROOT/usr/lib/crt0.o" \
        "$BUILD_DIR/user_sdl3_hello.o" \
        -Wl,--start-group \
        -lSDL3 -lwayland-client -lwayland-cursor -lxkbcommon -lffi \
        -lc -lm -lrt -lpthread -ldl \
        -Wl,--end-group \
        -o "$BUILD_DIR/user_sdl3_hello.elf"
fi

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
    "$KERNEL_DIR/debug"
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

# ── Kernel symbol table for the in-kernel stack walker ───────────────────
# DEBUGGING.md §5.3 requires symbolized backtraces in panic output.  The
# kernel.bin that actually runs is stripped to .text/.rodata/.data/.bss,
# so .symtab (present only in kernel.elf) isn't available at runtime.
# We materialize a sorted (addr, name) table as kernel_symbols.c.
#
# Two-pass link: a stub is compiled into the first kernel.elf so `nm`
# can extract addresses; we then regenerate kernel_symbols.c with the
# real table and re-link.  The second link's addresses drift by a
# few bytes from the first (the symbol table grew); a backtrace
# symbolized as `foo+0xNN` will thus show an offset a few bytes off.
# Acceptable per spec §5.3 ("let offline tooling resolve them").
KERNEL_SYMBOLS_C="$BUILD_DIR/kernel_symbols.c"
KERNEL_SYMBOLS_O="$BUILD_DIR/kernel_symbols.o"

cat > "$KERNEL_SYMBOLS_C" <<'EOF'
/* Auto-generated stub (pass 1).  Overwritten post-link with real data. */
#include "stackwalk.h"
const ksym_entry_t g_ksyms[] = { { 0, "_stub" } };
const size_t       g_ksyms_count = 1;
EOF
"$CLANG" "${KERNEL_CFLAGS[@]}" "${KERNEL_INCLUDES[@]}" \
    -c "$KERNEL_SYMBOLS_C" -o "$KERNEL_SYMBOLS_O"

"$LD" -nostdlib --no-dynamic-linker \
  -z max-page-size=0x1000 \
  -T "$KERNEL_DIR/link.ld" \
  "${KERNEL_ASM_OBJS[@]}" "${KERNEL_C_OBJS[@]}" "$KERNEL_SYMBOLS_O" \
  -o "$BUILD_DIR/kernel.elf"

# Extract T/t/D/d/B/b/R/r symbols and synthesize kernel_symbols.c.
# Sorted by address so stackwalk's binary search works.
# Filter out absolute-0 entries and the local $a / $d ARM markers (shouldn't
# exist on x86 but defensive).  Name length capped at 127 chars.
nm -n "$BUILD_DIR/kernel.elf" \
  | awk '
      /^[0-9a-f]+ [tTdDbBrR] / {
          addr = $1; name = $3;
          if (addr == "0000000000000000") next;
          if (length(name) > 127) name = substr(name, 1, 127);
          gsub(/"/, "\\\"", name);
          printf "    { 0x%s, \"%s\" },\n", addr, name;
      }
  ' > "$BUILD_DIR/kernel_symbols.inc"

{
  echo '/* Auto-generated pass-2 kernel symbol table. */'
  echo '#include "stackwalk.h"'
  echo 'const ksym_entry_t g_ksyms[] = {'
  cat  "$BUILD_DIR/kernel_symbols.inc"
  echo '};'
  echo 'const size_t g_ksyms_count = sizeof(g_ksyms) / sizeof(g_ksyms[0]);'
} > "$KERNEL_SYMBOLS_C"

"$CLANG" "${KERNEL_CFLAGS[@]}" "${KERNEL_INCLUDES[@]}" \
    -c "$KERNEL_SYMBOLS_C" -o "$KERNEL_SYMBOLS_O"

# Pass 2 link — the symbol object is bigger now; addresses shift by a
# few bytes but symbolic lookup tolerates the drift (see note above).
"$LD" -nostdlib --no-dynamic-linker \
  -z max-page-size=0x1000 \
  -T "$KERNEL_DIR/link.ld" \
  "${KERNEL_ASM_OBJS[@]}" "${KERNEL_C_OBJS[@]}" "$KERNEL_SYMBOLS_O" \
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
EXT2_SECTORS=262144  # 128 MiB — foot + xkb tree + wayland + ports ran out at 32 MiB
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
# -T bounds the FAT volume to the partition's actual sector count.
# Without it, mformat sized the filesystem to the END OF THE IMAGE
# (266240 sectors claimed inside a 2048-sector partition) and the FAT
# data region silently extended into the ext2 area: any file cluster
# past partition end was clobbered by the ext2 dd below.  kernel.bin
# crossing that line is exactly "FATAL: kernel read" + boot hang.
# With -T, an oversized kernel makes mcopy fail loudly instead.
ESP_SECTORS=$((ESP_END - ESP_START + 1))
mformat -i "$BUILD_DIR/disk.img"@@${ESP_OFFSET} -T ${ESP_SECTORS} -v "MAKAOS_ESP" ::
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
if [ -f "$BUILD_DIR/user_sdl3_hello.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl3_hello.elf" bin/sdl3_hello
    echo "[+] sdl3_hello ELF installed at bin/sdl3_hello (root:root 0755)"
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
    # /bin/sh is the universal POSIX shell entry point — dwl's `-s`
    # flag, wlroots/xdg-open helpers, dozens of ports do execl on
    # "/bin/sh -c …".  Install bash in-place as /bin/sh so those
    # paths work.  (Bash in sh-mode is spec-compliant enough for
    # everything we've hit so far.)
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_bash.elf" bin/sh
    echo "[+] sh (bash) installed at bin/sh (root:root 0755)"
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
# Wayland compositors — run from the shell prompt after login.
if [ -f "$SYSROOT/usr/bin/dwl" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/dwl" bin/dwl
    echo "[+] dwl ELF installed at bin/dwl (root:root 0755)"
fi
if [ -f "$SYSROOT/usr/bin/foot" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/foot" bin/foot
    echo "[+] foot ELF installed at bin/foot (root:root 0755)"
fi
if [ -f "$SYSROOT/usr/bin/tinywl" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/tinywl" bin/tinywl
    echo "[+] tinywl ELF installed at bin/tinywl (root:root 0755)"
fi
if [ -f "$SYSROOT/usr/bin/sway" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/sway"    bin/sway
    ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/swaymsg" bin/swaymsg
    # Default config: sway falls back to /etc/sway/config when the
    # user has none.  $term is foot; Mod4+Return spawns it.  The
    # `include /etc/sway/config.d/*` line is stripped: the glob can't
    # expand (wordexp has no pathname globbing yet, and the directory
    # doesn't exist on the image) and sway treats a failed include as
    # a fatal config error.
    # The wallpaper line is also dropped: sway was built with
    # -Ddefault-wallpaper=false, so the referenced PNG isn't installed
    # and sway treats an inaccessible background as a config error.
    sed -e 's|^include /etc/sway/config.d/\*|# include /etc/sway/config.d/* — disabled on MakaOS (no glob in wordexp yet)|' \
        -e 's|^output \* bg .*|# (default wallpaper not installed on MakaOS)|' \
        "$SYSROOT/etc/sway/config" > "$BUILD_DIR/etc_stage/sway_config"
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir etc/sway" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" \
        -R "write $BUILD_DIR/etc_stage/sway_config etc/sway/config" > /dev/null 2>&1 || true
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/sway        0040755 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/sway/config 0100644 0 0
    echo "[+] sway + swaymsg installed (+ /etc/sway/config)"
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

# ── Install DejaVu fonts + minimal fontconfig ──────────────────────
# foot uses fcft → fontconfig → freetype.  Without a fonts tree at the
# paths fontconfig scans, foot can't open any glyphs and its window
# stays blank — which is exactly the "dwl blank screen" symptom we hit
# (dwl has nothing to composite, so it sits on its initial clear-to-
# black frame).  Stage the host's DejaVu tree and write a minimal
# fonts.conf pointing fontconfig at /usr/share/fonts.
DEJAVU_SRC=/usr/share/fonts/truetype/dejavu
if [ -d "$DEJAVU_SRC" ]; then
    FONT_SCRIPT="$BUILD_DIR/fonts_install.debugfs"
    : > "$FONT_SCRIPT"
    # Stage the DejaVu dir tree inside the ext2 image.
    for d in usr usr/share usr/share/fonts usr/share/fonts/truetype \
             usr/share/fonts/truetype/dejavu \
             etc etc/fonts etc/fonts/conf.d \
             var var/cache var/cache/fontconfig \
             root/.cache root/.cache/fontconfig; do
        echo "mkdir /$d" >> "$FONT_SCRIPT"
    done
    for f in "$DEJAVU_SRC"/*.ttf; do
        [ -f "$f" ] || continue
        rel=$(basename "$f")
        echo "write $f /usr/share/fonts/truetype/dejavu/$rel" >> "$FONT_SCRIPT"
    done
    # Minimal fonts.conf — points fontconfig at /usr/share/fonts, sets
    # a writable cache under /var/cache/fontconfig.  Names 'monospace'
    # → DejaVu Sans Mono and 'sans-serif' → DejaVu Sans via <alias>
    # elements so foot's default font=monospace resolves without
    # shipping /etc/fonts/conf.d/*.conf from upstream fontconfig.
    FONTS_CONF="$BUILD_DIR/fonts.conf"
    # The <alias> shortcut has been unreliable in our port — fcft's
    # FcFontMatch kept returning "no match" for monospace despite the
    # DejaVu TTFs being indexed.  Use the full <match target="pattern">
    # form instead (what upstream /etc/fonts/conf.d/60-generic.conf
    # expands to).  Explicit > clever.
    cat > "$FONTS_CONF" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <!-- Both the top-level /usr/share/fonts and the leaf directory
       holding the actual TTFs are listed.  Fontconfig's recursive
       readdir wasn't descending into subdirs in the MakaOS ext2
       port (observed: opendir(/usr/share/fonts/) returns empty-
       looking iteration even though the tree is populated), so
       naming the leaf explicitly is the load-bearing line here. -->
  <dir>/usr/share/fonts</dir>
  <dir>/usr/share/fonts/truetype/dejavu</dir>
  <cachedir>/var/cache/fontconfig</cachedir>
  <cachedir>~/.cache/fontconfig</cachedir>

  <!-- Resolve generic family names to the DejaVu fonts we ship. -->

  <match target="pattern">
    <test qual="any" name="family"><string>monospace</string></test>
    <edit name="family" mode="prepend" binding="strong">
      <string>DejaVu Sans Mono</string>
    </edit>
  </match>
  <match target="pattern">
    <test qual="any" name="family"><string>mono</string></test>
    <edit name="family" mode="prepend" binding="strong">
      <string>DejaVu Sans Mono</string>
    </edit>
  </match>

  <match target="pattern">
    <test qual="any" name="family"><string>sans-serif</string></test>
    <edit name="family" mode="prepend" binding="strong">
      <string>DejaVu Sans</string>
    </edit>
  </match>
  <match target="pattern">
    <test qual="any" name="family"><string>sans</string></test>
    <edit name="family" mode="prepend" binding="strong">
      <string>DejaVu Sans</string>
    </edit>
  </match>

  <match target="pattern">
    <test qual="any" name="family"><string>serif</string></test>
    <edit name="family" mode="prepend" binding="strong">
      <string>DejaVu Serif</string>
    </edit>
  </match>

  <!-- Accept pattern — guarantees the final family list ends in a
       concrete font we ship, even if upstream substitution pipelines
       wipe our <match target="pattern"> edits. -->
  <match target="pattern">
    <test qual="any" name="family" compare="not_eq">
      <string>DejaVu Sans Mono</string>
    </test>
    <edit name="family" mode="append" binding="weak">
      <string>DejaVu Sans</string>
    </edit>
  </match>
</fontconfig>
EOF
    echo "write $FONTS_CONF /etc/fonts/fonts.conf" >> "$FONT_SCRIPT"

    # Foot config — override the default "monospace" family with the
    # concrete "DejaVu Sans Mono" name so we don't depend on the
    # fontconfig generic-family substitution working first-time.  The
    # file lives at /root/.config/foot/foot.ini because root is our
    # login user and foot searches $XDG_CONFIG_HOME (default: ~/.config).
    FOOT_INI="$BUILD_DIR/foot.ini"
    cat > "$FOOT_INI" <<'EOF'
# MakaOS default foot config.  Installed by build.sh into
# /root/.config/foot/foot.ini.  Overrides the upstream default
# font=monospace:size=8 with an explicit family so fcft's font match
# doesn't depend on fontconfig aliases resolving.
shell=/bin/bash
font=DejaVu Sans Mono:size=12
[main]
term=xterm-256color
EOF
    for d in root/.config root/.config/foot; do
        echo "mkdir /$d" >> "$FONT_SCRIPT"
    done
    echo "write $FOOT_INI /root/.config/foot/foot.ini" >> "$FONT_SCRIPT"

    debugfs -w "$BUILD_DIR/ext2.img" -f "$FONT_SCRIPT" > /dev/null 2>&1 || true
    # Directory perms (debugfs inherits root:root 0755 but set explicitly
    # so the scan matches Linux conventions the compositor expects).
    for d in /usr/share/fonts /usr/share/fonts/truetype \
             /usr/share/fonts/truetype/dejavu \
             /etc/fonts /etc/fonts/conf.d \
             /var/cache /var/cache/fontconfig; do
        ext2_setperm "$BUILD_DIR/ext2.img" "$d" 0040755 0 0
    done
    # /root/.cache/fontconfig — per-user cache, must be writable by root
    # (foot runs as root in our setup).  Fontconfig tries to mkdir this
    # on first run; if /root/.cache doesn't exist as a prior directory
    # it tries to create the whole chain.  Permissions are 0700 since
    # they're inside /root (which is 0700 anyway).
    ext2_setperm "$BUILD_DIR/ext2.img" /root/.cache             0040700 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /root/.cache/fontconfig  0040700 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /root/.config            0040700 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /root/.config/foot       0040700 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /root/.config/foot/foot.ini 0100644 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/fonts/fonts.conf 0100644 0 0
    font_count=$(ls "$DEJAVU_SRC"/*.ttf 2>/dev/null | wc -l)
    echo "[+] fonts installed: $font_count DejaVu .ttf → /usr/share/fonts/truetype/dejavu"
else
    echo "[!] DejaVu fonts not found on host ($DEJAVU_SRC) — foot will show blank"
fi

# ── Install xkeyboard-config data tree (/usr/share/X11/xkb) ──────────
# xkbcommon's xkb_keymap_new_from_names() walks this tree to compile
# the default keymap.  Without it, dwl/tinywl crash in xkb_context_ref
# after xkb_context_include_path_append_default silently fails.
# Stage the host's copy in one debugfs run (a `mkdir` + `write` per
# file would cost ~2000 subprocess spawns).
if [ -d /usr/share/X11/xkb ]; then
    XKB_SCRIPT="$BUILD_DIR/xkb_install.debugfs"
    : > "$XKB_SCRIPT"
    # Create /usr, /usr/share, /usr/share/X11, /usr/share/X11/xkb and
    # every subdirectory under it.  debugfs's `mkdir` fails if the
    # parent is missing, so emit in depth order.
    seen_dirs=""
    for d in usr usr/share usr/share/X11 usr/share/X11/xkb \
             $(cd /usr/share/X11/xkb && find . -type d | sed 's|^\./||' | sed 's|^|usr/share/X11/xkb/|' | grep -v '^usr/share/X11/xkb/\.$'); do
        echo "mkdir /$d" >> "$XKB_SCRIPT"
    done
    (cd /usr/share/X11/xkb && find . -type f) | while read -r rel; do
        rel="${rel#./}"
        echo "write /usr/share/X11/xkb/$rel /usr/share/X11/xkb/$rel"
    done >> "$XKB_SCRIPT"
    debugfs -w "$BUILD_DIR/ext2.img" -f "$XKB_SCRIPT" > /dev/null 2>&1 || true
    # Set perms on the root dir (files inherit 0644 default from debugfs write).
    ext2_setperm "$BUILD_DIR/ext2.img" /usr              0040755 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /usr/share        0040755 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /usr/share/X11    0040755 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /usr/share/X11/xkb 0040755 0 0
    echo "[+] xkeyboard-config installed at /usr/share/X11/xkb"
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
  -device virtio-gpu-pci \
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
