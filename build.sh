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
USER_CXX="$(pwd)/toolchain/bin/x86_64-pc-makaos-g++"   # for C++ ports (Mesa GL stack)
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

# Opt-in boot self-tests (chase-lev / slab+pcp stress / buddy uniqueness).
# Gated off by default because the slab stress test used to trip the PMM
# cross-CPU double-alloc (now fixed, mm/pcp 809127d).  Enable with
# `SELFTESTS=1 bash build.sh` to run them at boot.
if [ "${SELFTESTS:-0}" = "1" ]; then
  KERNEL_CFLAGS+=( -DMAKAOS_BOOT_SELFTESTS )
  echo "[build] boot self-tests ENABLED (-DMAKAOS_BOOT_SELFTESTS)"
fi

# `CONSOLE_SERIAL=1 bash build.sh` mirrors userland console output to the
# serial port — makes headless boots diagnosable and userland program/test
# output capturable.  Off by default (would flood the serial under a desktop).
if [ "${CONSOLE_SERIAL:-0}" = "1" ]; then
  KERNEL_CFLAGS+=( -DMAKAOS_CONSOLE_SERIAL )
  echo "[build] console->serial mirror ENABLED (-DMAKAOS_CONSOLE_SERIAL)"
fi
# SIGTEST=1 runs /bin/sigtest at boot (JVM SA_SIGINFO signal-layer verification).
if [ "${SIGTEST:-0}" = "1" ]; then
  KERNEL_CFLAGS+=( -DMAKAOS_SIGTEST )
  echo "[build] boot sigtest ENABLED (-DMAKAOS_SIGTEST)"
fi
# AVIANTEST=1 runs the Avian JVM at boot to execute Hello.class (first-light).
if [ "${AVIANTEST:-0}" = "1" ]; then
  KERNEL_CFLAGS+=( -DMAKAOS_AVIANTEST )
  echo "[build] boot Avian first-light ENABLED (-DMAKAOS_AVIANTEST)"
fi
# JDKTEST=1 stages the cross-built OpenJDK Zero java.base image + runs
# `/jdk/bin/java -version` at boot (behind -DMAKAOS_JDKTEST in main.c).
if [ "${JDKTEST:-0}" = "1" ]; then
  KERNEL_CFLAGS+=( -DMAKAOS_JDKTEST )
  echo "[build] boot OpenJDK 'java -version' ENABLED (-DMAKAOS_JDKTEST)"
fi

# ── User compile flags ─────────────────────────────────────────────────────
# -fPIE (position-independent) so every userland object -- libc especially --
# can link into a PIE (ET_DYN) for the dynamic loader.  -fPIE, NOT -fPIC: the
# main executable's own __thread TLS stays LOCAL-EXEC (TPOFF, resolved at link
# time); -fPIC would default __thread to general-dynamic and pull in
# __tls_get_addr (dynamic TLS -- a later phase).  PIE objects carry only
# R_X86_64_RELATIVE (GOT/data, applied by the kernel loader) + TPOFF relocs, no
# absolute R_X86_64_32/32S, so `ld -pie` accepts libc.a.  Apps still LINK as
# static ET_EXEC via the gcc driver (-static); PIE codegen runs fine at a fixed
# base, so this is transparent to them until they are linked -pie later.
USER_CFLAGS=(
  -ffreestanding -m64 -mno-red-zone
  -fPIE -fno-plt
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
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/fts.c" -o "$BUILD_DIR/user_fts.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/dns.c"   -o "$BUILD_DIR/user_dns.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/libc/math.c" -o "$BUILD_DIR/user_math.o"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/libc/math_c99.c" -o "$BUILD_DIR/user_math_c99.o"

# POSIX-header-aligned syscall wrapper files.  One file per header —
# each provides link-resolvable extern symbols for sysroot consumers
# (ports linked via -lc).  libc.h keeps its static-inline copies for
# in-tree apps that include it directly.
for src in unistd fcntl sys_stat sys_socket sys_eventfd sys_timerfd \
           sys_signalfd sys_ioctl sys_time sys_file sys_epoll sys_prctl sys_sysinfo sys_statfs sys_times sys_shm ifaddrs utmpx sched spawn sys_sendfile sys_inotify sys_xattr ssp jdk_compat glibc_compat \
           time arpa_inet string ctype makaos_input poll select signal resolv \
           getopt wordexp tls; do
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
   "$BUILD_DIR/user_math_c99.o" \
   "$BUILD_DIR/user_setjmp.o"  "$BUILD_DIR/user_pthread.o" \
   "$BUILD_DIR/user_pthread_tramp.o" \
   "$BUILD_DIR/user_dlfcn.o"    "$BUILD_DIR/user_locale.o" \
   "$BUILD_DIR/user_langinfo.o" "$BUILD_DIR/user_assert.o" \
   "$BUILD_DIR/user_stdlib.o"   "$BUILD_DIR/user_string.o" \
   "$BUILD_DIR/user_unistd.o"   "$BUILD_DIR/user_fcntl.o" \
   "$BUILD_DIR/user_fts.o" \
   "$BUILD_DIR/user_sys_stat.o" "$BUILD_DIR/user_sys_socket.o" \
   "$BUILD_DIR/user_sys_eventfd.o"  "$BUILD_DIR/user_sys_timerfd.o" \
   "$BUILD_DIR/user_sys_signalfd.o" "$BUILD_DIR/user_sys_ioctl.o" \
   "$BUILD_DIR/user_sys_time.o" "$BUILD_DIR/user_sys_file.o" \
   "$BUILD_DIR/user_sys_epoll.o" \
   "$BUILD_DIR/user_sys_prctl.o" "$BUILD_DIR/user_sys_sysinfo.o" "$BUILD_DIR/user_sys_statfs.o" "$BUILD_DIR/user_sys_times.o" "$BUILD_DIR/user_sys_shm.o" "$BUILD_DIR/user_ifaddrs.o" "$BUILD_DIR/user_utmpx.o" "$BUILD_DIR/user_sched.o" "$BUILD_DIR/user_spawn.o" "$BUILD_DIR/user_sys_sendfile.o" "$BUILD_DIR/user_sys_inotify.o" "$BUILD_DIR/user_sys_xattr.o" "$BUILD_DIR/user_ssp.o" "$BUILD_DIR/user_jdk_compat.o" "$BUILD_DIR/user_glibc_compat.o" \
   "$BUILD_DIR/user_time.o" "$BUILD_DIR/user_arpa_inet.o" \
   "$BUILD_DIR/user_ctype.o" "$BUILD_DIR/user_makaos_input.o" \
   "$BUILD_DIR/user_poll.o" "$BUILD_DIR/user_select.o" "$BUILD_DIR/user_signal.o" \
   "$BUILD_DIR/user_resolv.o" "$BUILD_DIR/user_getopt.o" \
   "$BUILD_DIR/user_wordexp.o" "$BUILD_DIR/user_tls.o" \
   "$BUILD_DIR/user_wayland_egl_stub.o" \
   "$BUILD_DIR/user_sdl_port_stubs.o" \
   "$BUILD_DIR/user_c11_threads.o" \
   "$BUILD_DIR/user_semaphore.o" \
   "$BUILD_DIR/user_wctype.o"

# crt0 — startup code sysroot-linked binaries get via STARTFILE_SPEC once
# the real cross-gcc is in place.  For the current host-gcc path we still
# list user_entry.o explicitly in each link line.
cp "$BUILD_DIR/user_entry.o" "$SYSROOT/usr/lib/crt0.o"

# crtdso.o — supplies a hidden, self-referential __dso_handle for shared
# objects built via `gcc -shared` (C++ static destructors reference it via
# __cxa_atexit, and MakaOS links .so's with no crt fragments).  The cross-gcc
# startfile spec selects crtdso.o for -shared and crt0.o otherwise.
"$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" -fPIC \
    -c "$USERLAND_DIR/libc/crtdso.c" -o "$SYSROOT/usr/lib/crtdso.o"

# Default link script for sysroot users.
cp "$USERLAND_DIR/link.ld" "$SYSROOT/usr/lib/makaos-link.ld"
# PIE link script for sysroot users that link dynamic (-pie) executables via the
# gcc driver -- e.g. the OpenJDK launchers.  The driver's specs reference this at
# %R/usr/lib/link-pie.ld for the -pie case (see toolchain specs).
cp "$USERLAND_DIR/link-pie.ld" "$SYSROOT/usr/lib/link-pie.ld"

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
    "$BUILD_DIR/user_tls.o"
    "$BUILD_DIR/user_libc.o"
    "$BUILD_DIR/user_stdio.o"
    "$BUILD_DIR/user_dns.o"
    "$BUILD_DIR/user_math.o"
    "$BUILD_DIR/user_setjmp.o"
    # POSIX-header-split extern wrappers — needed for in-tree apps now
    # that libc.h declares these as prototypes rather than inlining them.
    "$BUILD_DIR/user_unistd.o"
    "$BUILD_DIR/user_fcntl.o"
    "$BUILD_DIR/user_fts.o"
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
    "$BUILD_DIR/user_select.o"
    "$BUILD_DIR/user_signal.o"
    "$BUILD_DIR/user_arpa_inet.o"
    "$BUILD_DIR/user_string.o"
    "$BUILD_DIR/user_stdlib.o"
    "$BUILD_DIR/user_dlfcn.o"
    "$BUILD_DIR/user_pthread.o"        # dlfcn's loader lock -> pthread_mutex_*
    "$BUILD_DIR/user_pthread_tramp.o"  # pthread.o -> pthread_trampoline
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

# sigtest — verifies SA_SIGINFO delivery + ucontext RIP-redirect (JVM signal
# layer).  Test-only: compiled + installed + boot-spawned only under SIGTEST=1.
if [ "${SIGTEST:-0}" = "1" ]; then
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/sigtest/sigtest.c" -o "$BUILD_DIR/user_sigtest.o"
# Link the fresh in-tree pthread objects (they carry the new pthread_kill +
# sched_yield); the linker pulls the rest from the toolchain libc archive.
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_sigtest.o" \
   "$BUILD_DIR/user_pthread.o" "$BUILD_DIR/user_pthread_tramp.o" \
   -o "$BUILD_DIR/user_sigtest.elf"
fi

# virgltest -- drives the virtio-gpu 3D render-node uAPI (/dev/dri/renderD128)
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/virgltest/virgltest.c" -o "$BUILD_DIR/user_virgltest.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_virgltest.o" \
   -o "$BUILD_DIR/user_virgltest.elf"

# gltest -- first light through the ported Mesa GL stack (EGL + GLES2 + gbm +
# the static virgl gallium driver).  Only build it if the Mesa libs are ported
# into the sysroot (scripts/port-mesa.sh); otherwise skip cleanly.
GL_L="$SYSROOT/usr/lib"
if [ -f "$GL_L/libEGL.a" ] && [ -f "$GL_L/dri/virtio_gpu_dri.so" ]; then
    # Compile against the sysroot public + Mesa headers only (NOT userland/libc,
    # whose internal size_t typedef conflicts with the ABI stddef.h).
    "$USER_CC" -ffreestanding -m64 -mno-red-zone -fno-pie -fno-pic -fno-plt \
        -fno-stack-protector -fno-builtin -O0 -Wall -isystem "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/gltest/gltest.c" -o "$BUILD_DIR/user_gltest.o"
    # Link with g++ (libstdc++ for Mesa's C++), the megadriver + Mesa archives in
    # one group (they cross-reference), libffi after wayland, allow-multiple-def
    # for the math functions Mesa ships that MakaOS's libc also provides.
    # -s (strip) + --gc-sections: the unstripped static Mesa binary is ~80 MB
    # (mostly debug info); stripped it is ~15 MB.
    "$USER_CXX" --sysroot="$SYSROOT" -m64 -mno-red-zone -no-pie -s \
        "$BUILD_DIR/user_gltest.o" \
        -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--start-group \
        "$GL_L/dri/virtio_gpu_dri.so" \
        "$GL_L/libEGL.a" "$GL_L/libGLESv2.a" "$GL_L/libgbm.a" "$GL_L/libglapi.a" \
        "$GL_L/libdrm.a" "$GL_L/libexpat.a" \
        "$GL_L/libwayland-client.a" "$GL_L/libwayland-server.a" "$GL_L/libffi.a" \
        -Wl,--end-group \
        "$GL_L/libz.a" -lm -lpthread \
        -o "$BUILD_DIR/user_gltest.elf"
    echo "[build] gltest linked against the static Mesa GL stack"
fi

# glfwtest -- GLFW-Wayland smoke test (the windowing path Minecraft's LWJGL needs).
# Links the ported GLFW static lib on top of the same Mesa GL stack as gltest,
# plus the Wayland client libs GLFW's backend uses (cursor + egl-window + xkb).
# Built only if GLFW is ported (scripts/port-glfw.sh) and the Mesa libs exist.
if [ -f "$GL_L/libglfw3.a" ] && [ -f "$GL_L/libEGL.a" ] && [ -f "$GL_L/dri/virtio_gpu_dri.so" ]; then
    # PIE + --export-dynamic so the wayland/EGL/GLES/xkb symbols GLFW resolves via
    # its dlopen fallback (dlsym(RTLD_DEFAULT,...) -- MakaOS ships those libs static,
    # not as dlopen-able .so) live in glfwtest's own dynamic symbol table.
    # --whole-archive forces those symbols in (GLFW references them only through
    # dlsym'd pointers, so they would not otherwise be pulled). libwayland-server.a
    # resolves the server-side symbols the whole-archived EGL wayland-drm pulls in.
    "$USER_CC" -fPIE -ffreestanding -m64 -mno-red-zone \
        -fno-stack-protector -fno-builtin -O0 -Wall -isystem "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/glfwtest/glfwtest.c" -o "$BUILD_DIR/user_glfwtest.o"
    "$USER_CXX" --sysroot="$SYSROOT" -m64 -mno-red-zone -pie -Wl,--export-dynamic -s \
        "$BUILD_DIR/user_glfwtest.o" \
        -Wl,--allow-multiple-definition \
        -Wl,--whole-archive \
            "$GL_L/libwayland-client.a" "$GL_L/libwayland-cursor.a" "$GL_L/libwayland-egl.a" \
            "$GL_L/libxkbcommon.a" "$GL_L/libEGL.a" "$GL_L/libGLESv2.a" \
        -Wl,--no-whole-archive \
        -Wl,--start-group \
            "$GL_L/libglfw3.a" "$GL_L/dri/virtio_gpu_dri.so" "$GL_L/libgbm.a" "$GL_L/libglapi.a" \
            "$GL_L/libdrm.a" "$GL_L/libexpat.a" "$GL_L/libffi.a" "$GL_L/libwayland-server.a" \
        -Wl,--end-group \
        "$GL_L/libz.a" -lm -lpthread \
        -o "$BUILD_DIR/user_glfwtest.elf"
    echo "[build] glfwtest linked (PIE+export-dynamic) against GLFW-Wayland + Mesa GL"

    # libglfw.so -- a self-contained MakaOS shared lib for LWJGL (Minecraft) to
    # dlopen from the JVM. Whole-archives GLFW + the wayland/EGL/GLES/xkb symbols
    # (exported in its own dynsym so GLFW's dlsym(RTLD_DEFAULT,...) fallback finds
    # them) + Mesa/virgl; only libc stays undefined (resolved at load against the
    # java process). --hash-style=both so MakaOS's loader reads DT_HASH.
    # GLX proc-loader shim (glXGetProcAddress -> Mesa eglGetProcAddress) so LWJGL
    # can resolve the full desktop-GL entry-point set from this same Mesa state.
    "$USER_CC" --sysroot="$SYSROOT" -m64 -mno-red-zone -fPIC -O2 \
        -c "$USERLAND_DIR/gl/gl_glx_shim.c" -o "$BUILD_DIR/gl_glx_shim.o"
    "$USER_CXX" --sysroot="$SYSROOT" -m64 -mno-red-zone -shared -fPIC \
        -Wl,--hash-style=both -Wl,-soname,libglfw.so -Wl,--allow-multiple-definition \
        "$BUILD_DIR/gl_glx_shim.o" \
        -Wl,--whole-archive \
            "$GL_L/libglfw3.a" \
            "$GL_L/libwayland-client.a" "$GL_L/libwayland-cursor.a" "$GL_L/libwayland-egl.a" \
            "$GL_L/libxkbcommon.a" "$GL_L/libEGL.a" "$GL_L/libGLESv2.a" \
        -Wl,--no-whole-archive \
        -Wl,--start-group \
            "$GL_L/dri/virtio_gpu_dri.so" "$GL_L/libgbm.a" "$GL_L/libglapi.a" \
            "$GL_L/libdrm.a" "$GL_L/libexpat.a" "$GL_L/libffi.a" "$GL_L/libwayland-server.a" \
            "$GL_L/libz.a" \
        -Wl,--end-group \
        -o "$BUILD_DIR/libglfw.so"
    echo "[build] libglfw.so (self-contained Wayland GLFW for LWJGL) built"
fi

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/helloraw/helloraw.c" -o "$BUILD_DIR/user_helloraw.o"
ld -nostdlib -T "$USER_LINK" --entry=_start "$BUILD_DIR/user_helloraw.o" \
   -o "$BUILD_DIR/user_helloraw.elf"
"$OBJCOPY" -O binary "$BUILD_DIR/user_helloraw.elf" "$BUILD_DIR/user_helloraw.bin"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/test_posix1/test_posix1.c" -o "$BUILD_DIR/user_test_posix1.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_test_posix1.o" \
   -o "$BUILD_DIR/user_test_posix1.elf"

# unixsock_test — isolates which AF_UNIX/epoll syscall wl_display_add_socket_auto hangs on
if [ -f "$USERLAND_DIR/apps/unixsock_test/unixsock_test.c" ]; then
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/unixsock_test/unixsock_test.c" -o "$BUILD_DIR/user_unixsock_test.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_unixsock_test.o" \
   -o "$BUILD_DIR/user_unixsock_test.elf"
fi

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/test_io_uring/test_io_uring.c" -o "$BUILD_DIR/user_test_io_uring.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_test_io_uring.o" \
   -o "$BUILD_DIR/user_test_io_uring.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/smp_test/smp_test.c" -o "$BUILD_DIR/user_smp_test.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_smp_test.o" \
   -o "$BUILD_DIR/user_smp_test.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/heapalias/heapalias.c" -o "$BUILD_DIR/user_heapalias.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_heapalias.o" \
   -o "$BUILD_DIR/user_heapalias.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/apps/makaclock/makaclock.c" -o "$BUILD_DIR/user_makaclock.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_makaclock.o" \
   -o "$BUILD_DIR/user_makaclock.elf"

"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/apps/tone/tone.c" -o "$BUILD_DIR/user_tone.o"
"$USER_CC" "${USER_CFLAGS[@]}" "$BUILD_DIR/user_tone.o" \
   -o "$BUILD_DIR/user_tone.elf"

# ── pietest -- PIE self-relocation smoke test (dynamic-loader Phase 0/1) ──
# Linked via ld DIRECTLY: gcc's driver forces -static (overriding -pie -> ET_EXEC),
# but ld -pie yields a proper ET_DYN + PT_DYNAMIC + R_X86_64_RELATIVE that the
# kernel ELF loader relocates at load time.  Freestanding, no libc.
"$USER_CC" -fPIC -ffreestanding -m64 -mno-red-zone -fno-stack-protector \
   -c "$USERLAND_DIR/apps/pietest/pietest.c" -o "$BUILD_DIR/user_pietest.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -pie --export-dynamic -e _start \
   --build-id=none "$BUILD_DIR/user_pietest.o" -o "$BUILD_DIR/user_pietest.elf"

# ── piehello -- Phase 0 milestone: a REAL libc program as a static PIE ────
# Compiled -fPIE (via USER_CFLAGS) and linked with crt0 (_entry) + the now-PIC
# libc.a via `ld -pie` directly (gcc's driver forces -static -T fixed-base
# makaos-link.ld -> ET_EXEC).  We use link-pie.ld (base-0 ET_DYN, the stock -pie
# layout + libc's __tdata_end/__tbss_end TLS symbols) so PT_DYNAMIC carries the
# RELATIVE relocs the kernel applies and crt0's TLS/init_array symbols resolve.
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
   -c "$USERLAND_DIR/apps/piehello/piehello.c" -o "$BUILD_DIR/user_piehello.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -m elf_x86_64_makaos -pie \
   --export-dynamic -e _entry --build-id=none -T "$USERLAND_DIR/link-pie.ld" \
   "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_piehello.o" \
   --start-group "$SYSROOT/usr/lib/libc.a" --end-group \
   -o "$BUILD_DIR/user_piehello.elf"

# ── dlopen (dynamic loader Phase 2) ──────────────────────────────────────
# libdso.so: a no-import shared object built with `ld -shared` (stock -shared
# script produces ET_DYN + SysV HASH + dynsym).  dltest: a PIE that dlopen()s
# it via the real libc loader, dlsym()s + calls into it, then dlclose()s.
# libdso2.so: a dependency of libdso.so (soname libdso2.so -> DT_NEEDED).
"$USER_CC" -fPIC -ffreestanding -m64 -mno-red-zone -fno-stack-protector \
   -c "$USERLAND_DIR/apps/dsotest2/dsotest2.c" -o "$BUILD_DIR/user_dsotest2.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -shared -m elf_x86_64_makaos \
   -soname libdso2.so --build-id=none "$BUILD_DIR/user_dsotest2.o" -o "$BUILD_DIR/libdso2.so"
# libdso.so: links against libdso2.so -> gets a DT_NEEDED the loader must satisfy.
"$USER_CC" -fPIC -ffreestanding -m64 -mno-red-zone -fno-stack-protector \
   -c "$USERLAND_DIR/apps/dsotest/dsotest.c" -o "$BUILD_DIR/user_dsotest.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -shared -m elf_x86_64_makaos \
   --build-id=none "$BUILD_DIR/user_dsotest.o" "$BUILD_DIR/libdso2.so" -o "$BUILD_DIR/libdso.so"
# libdsobad.so: imports a strong undefined symbol -> dlopen must reject it.
"$USER_CC" -fPIC -ffreestanding -m64 -mno-red-zone -fno-stack-protector \
   -c "$USERLAND_DIR/apps/dsobad/dsobad.c" -o "$BUILD_DIR/user_dsobad.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -shared -m elf_x86_64_makaos \
   --build-id=none "$BUILD_DIR/user_dsobad.o" -o "$BUILD_DIR/libdsobad.so"
# libtlsdso.so: has a __thread var -> general-dynamic TLS (__tls_get_addr + DTPMOD64).
"$USER_CC" -fPIC -ffreestanding -m64 -mno-red-zone -fno-stack-protector \
   -c "$USERLAND_DIR/apps/tlsdso/tlsdso.c" -o "$BUILD_DIR/user_tlsdso.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -shared -m elf_x86_64_makaos \
   --build-id=none "$BUILD_DIR/user_tlsdso.o" -o "$BUILD_DIR/libtlsdso.so"
"$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
   -c "$USERLAND_DIR/apps/dltest/dltest.c" -o "$BUILD_DIR/user_dltest.o"
"$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -m elf_x86_64_makaos -pie \
   --export-dynamic -e _entry --build-id=none -T "$USERLAND_DIR/link-pie.ld" \
   "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_dltest.o" \
   --start-group "$SYSROOT/usr/lib/libc.a" --end-group \
   -o "$BUILD_DIR/user_dltest.elf"

# ── SDL3 apps ────────────────────────────────────────────────────────
# libSDL3.a now uses the static-EGL path (no dlopen; SDL_egl.o references the
# EGL entry points directly), so EVERY SDL video app pulls SDL_egl.o and must
# link the full static Mesa stack via g++ (libstdc++ for Mesa's C++) + crt0.o
# (argv + .init_array for Mesa's C++ ctors).  One shared link recipe:
link_sdl_app() {   # $1 = basename: build/user_$1.o -> build/user_$1.elf
    "$USER_CXX" --sysroot="$SYSROOT" -m64 -mno-red-zone -no-pie -s \
        -nostartfiles -Wl,--build-id=none \
        "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_$1.o" \
        -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--start-group \
        "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" \
        -lSDL3 \
        -lEGL -lGLESv2 -lGLESv1_CM -lgbm -lglapi \
        -lwayland-egl -lwayland-client -lwayland-cursor -lwayland-server \
        -lxkbcommon -lffi -lexpat -ldrm \
        -lc -lm -lrt -lpthread -ldl -lz \
        -Wl,--end-group \
        -o "$BUILD_DIR/user_$1.elf"
}

# sdl3_hello -- end-to-end SDL3 smoke test (software renderer via wl_shm).
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
        -I "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/sdl3_hello/sdl3_hello.c" \
        -o "$BUILD_DIR/user_sdl3_hello.o"
    link_sdl_app sdl3_hello
fi

# ── sdl3_gl -- SDL3 GLES2/EGL (Wayland-EGL -> Mesa -> virgl) smoke test ───────
# Proves client-side hardware GL: a GLES2 context whose buffer sway composites
# via linux-dmabuf.  Needs the full static Mesa stack (megadriver + EGL/GLES2/
# gbm/glapi) in addition to SDL3, so it links via g++ (libstdc++ for Mesa's C++)
# with --allow-multiple-definition (Mesa ships math symbols libc also provides)
# and crt0.o (argv + .init_array for Mesa's C++ global ctors), like gltest.
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
        -I "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/sdl3_gl/sdl3_gl.c" \
        -o "$BUILD_DIR/user_sdl3_gl.o"
    link_sdl_app sdl3_gl
    echo "[build] sdl3_gl linked against SDL3 + the static Mesa GL stack"
fi

# sdl3_gl3 -- DESKTOP GL 3.3 core smoke test (proves virgl exposes desktop GL,
# not just GLES2 -- i.e. no GL4ES translation layer needed for legacy/core GL).
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
        -I "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/sdl3_gl3/sdl3_gl3.c" \
        -o "$BUILD_DIR/user_sdl3_gl3.o"
    link_sdl_app sdl3_gl3
    echo "[build] sdl3_gl3 (desktop GL 3.3 core) linked"
fi

# ── sdl3_audio -- end-to-end SDL3 audio smoke test (native /dev/dsp backend) ──
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    "$USER_CC" "${USER_CFLAGS[@]}" "${SYSROOT_CFLAGS[@]}" \
        -I "$SYSROOT/usr/include" \
        -c "$USERLAND_DIR/apps/sdl3_audio/sdl3_audio.c" \
        -o "$BUILD_DIR/user_sdl3_audio.o"
    link_sdl_app sdl3_audio
fi

# Ensure the toolchain's libstdc++/libsupc++ are PIC (a from-source toolchain may
# have built them non-PIC).  Self-guards: a no-op if already PIC.  PIC C++ links
# fine into the static -no-pie apps too, so this is the ONE C++ runtime.  Only
# bother when the Mesa/SDL stack is present (i.e. GL builds are live).
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    SYSROOT="$SYSROOT" bash "$(pwd)/scripts/build-libstdcxx-pic.sh" || \
        echo "[build] WARNING: PIC libstdc++ retrofit failed; libSDL3.so may not link"
fi

# ── libSDL3.so + sdltest -- the real dynamic-loader milestone (Phase 5) ──────
# A SELF-CONTAINED shared object: SDL3 (whole-archive) + the whole Mesa GL stack
# + wayland + the (now PIC) C++ runtime (libstdc++/supc++/gcc), importing ONLY
# libc from the host exe.  sdltest is a PIE that dlopen()s it and calls SDL entry
# points -- proving the loader on a real 83 MB object with ~26k relocs, deps
# folded in, and cross-module general-dynamic TLS (libc's errno).
if [ -f "$SYSROOT/usr/lib/libSDL3.a" ] && [ -f "$SYSROOT/usr/lib/dri/virtio_gpu_dri.so" ]; then
    echo "[build] linking libSDL3.so (self-contained shared object)"
    bash "$(pwd)/scripts/link-libsdl3-so.sh" "$BUILD_DIR/libSDL3.so"
    # sdltest PIE provides libc (+ the errno TLS var) to the .so via
    # --export-dynamic; whole-archive libc.a so EVERY symbol libSDL3.so imports
    # is present in the exe's exported .dynsym (RTLD_NOW resolves them all).
    "$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
        -c "$USERLAND_DIR/apps/sdltest/sdltest.c" -o "$BUILD_DIR/user_sdltest.o"
    "$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -m elf_x86_64_makaos -pie \
        --export-dynamic -e _entry --build-id=none -T "$USERLAND_DIR/link-pie.ld" \
        "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_sdltest.o" \
        --whole-archive "$SYSROOT/usr/lib/libc.a" --no-whole-archive \
        -o "$BUILD_DIR/user_sdltest.elf"
    echo "[build] sdltest PIE linked (dlopen libSDL3.so)"

    # sdl2-compat (libSDL2.a) + sdl2test: the SDL2 API over SDL3.  sdl2-compat
    # loads libSDL3.so.0 at runtime via dlopen+dlsym (unmodified upstream path,
    # now that the loader exists), so an SDL2 program reaches SDL3 with no hack.
    SYSROOT="$SYSROOT" bash "$(pwd)/scripts/port-sdl2-compat.sh"
    if [ -f "$SYSROOT/usr/lib/libSDL2.a" ]; then
        "$USER_CC" "${USER_CFLAGS[@]}" -D_REENTRANT -isystem "$SYSROOT/usr/include" \
            "${USER_INCLUDES[@]}" \
            -c "$USERLAND_DIR/apps/sdl2test/sdl2test.c" -o "$BUILD_DIR/user_sdl2test.o"
        "$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -m elf_x86_64_makaos -pie \
            --export-dynamic -e _entry --build-id=none -T "$USERLAND_DIR/link-pie.ld" \
            "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_sdl2test.o" \
            "$SYSROOT/usr/lib/libSDL2.a" \
            --whole-archive "$SYSROOT/usr/lib/libc.a" --no-whole-archive \
            -o "$BUILD_DIR/user_sdl2test.elf"
        echo "[build] sdl2test PIE linked (SDL2 -> sdl2-compat -> dlopen libSDL3)"
    fi

    # DarkPlaces engine (Xonotic): a PIE that links sdl2-compat (libSDL2.a) +
    # the codec libs and dlopens libSDL3.so.0 at runtime for video/GL/input.
    if [ -f "$SYSROOT/usr/lib/libSDL2.a" ] && [ -d "$(pwd)/build/third_party/darkplaces" ]; then
        SYSROOT="$SYSROOT" bash "$(pwd)/scripts/port-darkplaces.sh" || \
            echo "[build] WARNING: DarkPlaces build failed"
        # dplaunch: forces SDL's offscreen video driver + basedir, execs
        # darkplaces (autologin can't pass args/env) -- headless GL bring-up.
        if [ -f "$BUILD_DIR/darkplaces_build/darkplaces.elf" ]; then
            "$USER_CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" \
                -c "$USERLAND_DIR/apps/dplaunch/dplaunch.c" -o "$BUILD_DIR/user_dplaunch.o"
            "$(pwd)/toolchain/bin/x86_64-pc-makaos-ld" -m elf_x86_64_makaos -pie \
                --export-dynamic -e _entry --build-id=none -T "$USERLAND_DIR/link-pie.ld" \
                "$SYSROOT/usr/lib/crt0.o" "$BUILD_DIR/user_dplaunch.o" \
                --start-group "$SYSROOT/usr/lib/libc.a" --end-group \
                -o "$BUILD_DIR/user_dplaunch.elf"
        fi
    fi
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
for util in cat echo mkdir rm mv clear reboot uname; do
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
EXT2_SECTORS=1048576  # 512 MiB — DE stack (sway+swaybar+swaybg+tofi) + xkb tree + the 83 MB libSDL3.so
# JDKTEST stages the ~55 MB exploded OpenJDK java.base image (6437 class files +
# libjvm.so); grow the rootfs to 1 GiB so it fits with headroom.
if [ "${JDKTEST:-0}" = "1" ] || [ "${LWJGLTEST:-0}" = "1" ]; then EXT2_SECTORS=2097152; fi  # 1 GiB
# MCTEST stages the full exploded JDK (180 MB) + Minecraft client/libs/assets
# (~715 MB) + writable gameDir; grow the rootfs to 3 GiB. Default mke2fs inode
# ratio (16 KiB) gives ~196k inodes for ~30k files -- ample.
if [ "${MCTEST:-0}" = "1" ]; then EXT2_SECTORS=6291456; fi  # 3 GiB
# Stress test needs the big-fs conditions (384 block groups, high inodes) where
# the leaves[] corruption reproduced.
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
mkdir lib
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
ext2_setperm "$BUILD_DIR/ext2.img" /lib      0040755 0 0      # /lib root:root rwxr-xr-x
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

# Optional auto-login (test/kiosk only, agetty -a analog).  `AUTOLOGIN=1 bash
# build.sh` installs /etc/autologin so login starts a session without an
# interactive prompt — lets the headless boot-test harness reach the
# compositor deterministically without blind console keystrokes.  Default
# builds omit the file, so normal boots still prompt for credentials.
# Format: "user" or "user:command" (command replaces the login shell).
if [ "${AUTOLOGIN:-0}" = "1" ]; then
  printf '%s\n' "${AUTOLOGIN_SPEC:-root:/bin/sway}" > "$BUILD_DIR/etc_stage/autologin"
  debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/autologin etc/autologin" > /dev/null 2>&1 || true
  ext2_setperm "$BUILD_DIR/ext2.img" /etc/autologin 0100600 0 0
  echo "[build] AUTOLOGIN enabled: /etc/autologin = ${AUTOLOGIN_SPEC:-root:/bin/sway}"
fi

# Put a secret file in /root to test permission enforcement
echo "root's secret — maka cannot read this" > "$BUILD_DIR/etc_stage/root_secret.txt"
debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/etc_stage/root_secret.txt root/secret.txt" > /dev/null 2>&1 || true
ext2_setperm "$BUILD_DIR/ext2.img" /root/secret.txt 0100600 0 0

# Quake shareware data for DarkPlaces (DarkPlaces-Quake / gamedir id1): install
# build/quake-id1/pak0.pak (staged by scripts/fetch-quake-shareware.sh) so
# `dplaunch` has content to render.  Absent unless fetched -- default images ship
# without it.  Run headless via scripts/run-gl.sh (autologin root:/bin/dplaunch).
if [ -f "$BUILD_DIR/quake-id1/pak0.pak" ]; then
  debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /root/id1" > /dev/null 2>&1 || true
  ext2_setperm "$BUILD_DIR/ext2.img" /root/id1 0040755 0 0
  debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/quake-id1/pak0.pak root/id1/pak0.pak" > /dev/null 2>&1 || true
  ext2_setperm "$BUILD_DIR/ext2.img" /root/id1/pak0.pak 0100644 0 0
  echo "[build] installed Quake shareware pak0.pak at /root/id1 (DarkPlaces data)"
  # Optional loose autoexec.cfg (diagnostic cvars etc.) if present.
  if [ -f "$BUILD_DIR/quake-id1/autoexec.cfg" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "rm /root/id1/autoexec.cfg" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/quake-id1/autoexec.cfg root/id1/autoexec.cfg" > /dev/null 2>&1 || true
    ext2_setperm "$BUILD_DIR/ext2.img" /root/id1/autoexec.cfg 0100644 0 0
  fi
fi

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

# ext2_install_tree <img> <src_dir> <dst_prefix>
# Recursively stage a host directory tree into the ext2 image under /<dst_prefix>
# using ONE debugfs session (mkdir every subdir parent-first, then write every
# file).  Far faster than per-file debugfs invocations for large trees (the JDK's
# 6437-file java.base module).  dst_prefix has no leading slash.
ext2_install_tree() {
    local img="$1" src="$2" dstpref="$3"
    src="$(cd "$src" && pwd)"   # absolutize for debugfs `write`
    local script; script="$(mktemp)"
    {
        echo "mkdir /$dstpref"
        ( cd "$src" && find . -mindepth 1 -type d | LC_ALL=C sort ) \
            | sed "s|^\./|mkdir /$dstpref/|"
        # -L: follow symlinks so a symlink-to-file (e.g. lib/jvm.cfg) is reported
        # as -type f and `write` copies its target's content.
        ( cd "$src" && find -L . -type f ) \
            | while IFS= read -r f; do
                  rel="${f#./}"
                  printf 'write %s/%s /%s/%s\n' "$src" "$rel" "$dstpref" "$rel"
              done
    } > "$script"
    debugfs -w "$img" -f "$script" > /dev/null 2>&1 || true
    rm -f "$script"
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
if [ "${SIGTEST:-0}" = "1" ] && [ -f "$BUILD_DIR/user_sigtest.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sigtest.elf" bin/sigtest
fi
# Avian JVM first-light (AVIANTEST=1): install the cross-built avian VM +
# classpath.jar + Hello.class, spawned at boot by main.c (behind MAKAOS_AVIANTEST).
if [ "${AVIANTEST:-0}" = "1" ] && [ -f "$BUILD_DIR/avian-app/avian" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/avian-app/avian" bin/avian
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /avian" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/avian-app/classpath.jar avian/classpath.jar" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/avian-app/Hello.class avian/Hello.class" > /dev/null 2>&1 || true
    echo "[build] Avian JVM installed: /bin/avian + /avian/{classpath.jar,Hello.class}"
fi
# OpenJDK Zero java.base (JDKTEST=1): stage the exploded runtime image at /jdk so
# `/jdk/bin/java -version` runs at boot.  Only bin/java + lib/ + the java.base
# exploded classes are staged (not the full images/ tree with every module).
if [ "${JDKTEST:-0}" = "1" ] || [ "${LWJGLTEST:-0}" = "1" ] || [ "${MCTEST:-0}" = "1" ]; then
    # JDK_VARIANT=server selects the tiered C1+C2 JIT VM; default zero (interpreter).
    JDK_IMG="build/third_party/openjdk17u/build/linux-x86_64-${JDK_VARIANT:-zero}-release/jdk"
    if [ -x "$JDK_IMG/bin/java" ]; then
        debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /jdk"         > /dev/null 2>&1 || true
        debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /jdk/bin"     > /dev/null 2>&1 || true
        debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /jdk/modules" > /dev/null 2>&1 || true
        ext2_install_bin "$BUILD_DIR/ext2.img" "$JDK_IMG/bin/java" jdk/bin/java
        ext2_install_tree "$BUILD_DIR/ext2.img" "$JDK_IMG/lib"                jdk/lib
        # The JDK's libzip.so is linked against system zlib (libz.so.1), needed
        # by java.util.zip.Inflater -> reading ANY jar (client.jar, libraries).
        # The zlib port only builds libz.a (static); build a SHARED libz.so.1
        # from the same pinned, already-PIC sources with the MakaOS cross-gcc and
        # stage it at /lib/libz.so.1 (the loader's search path).
        ZLIB_SRC_DIR="$BUILD_DIR/third_party/zlib-1.3.1"
        if [ -d "$ZLIB_SRC_DIR" ] && [ -x "$USER_CC" ]; then
            "$USER_CC" -shared -O2 -fPIC -DHAVE_HIDDEN -DZ_HAVE_UNISTD_H \
                -Wl,-soname,libz.so.1 -o "$BUILD_DIR/libz.so.1" \
                "$ZLIB_SRC_DIR"/adler32.c "$ZLIB_SRC_DIR"/compress.c "$ZLIB_SRC_DIR"/crc32.c \
                "$ZLIB_SRC_DIR"/deflate.c "$ZLIB_SRC_DIR"/gzclose.c "$ZLIB_SRC_DIR"/gzlib.c \
                "$ZLIB_SRC_DIR"/gzread.c "$ZLIB_SRC_DIR"/gzwrite.c "$ZLIB_SRC_DIR"/infback.c \
                "$ZLIB_SRC_DIR"/inffast.c "$ZLIB_SRC_DIR"/inflate.c "$ZLIB_SRC_DIR"/inftrees.c \
                "$ZLIB_SRC_DIR"/trees.c "$ZLIB_SRC_DIR"/uncompr.c "$ZLIB_SRC_DIR"/zutil.c \
                2>/dev/null && ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libz.so.1" lib/libz.so.1 \
                && echo "[build] libz.so.1 (shared zlib for the JDK) staged at /lib" \
                || echo "[build] warn: libz.so.1 build/stage failed -- jar reads will fail"
        fi
        # conf/ holds java.security (JCA provider list + policy) which the
        # security subsystem loads on first crypto/SecureRandom use -- Minecraft
        # (and any ResourceBundle/locale path) needs it, or class init cascades
        # into NoClassDefFoundError: sun.security.jca.Providers.
        [ -d "$JDK_IMG/conf" ] && ext2_install_tree "$BUILD_DIR/ext2.img" "$JDK_IMG/conf" jdk/conf
        # Stage every built exploded module (java.base, jdk.unsupported for
        # sun.misc.Unsafe which LWJGL/Minecraft need, and any others built).
        for moddir in "$JDK_IMG/modules"/*/; do
            [ -d "$moddir" ] || continue
            ext2_install_tree "$BUILD_DIR/ext2.img" "$moddir" "jdk/modules/$(basename "$moddir")"
        done
        [ -f "$JDK_IMG/release" ] && debugfs -w "$BUILD_DIR/ext2.img" -R "write $JDK_IMG/release /jdk/release" > /dev/null 2>&1 || true
        # Hello.class -- the `java Hello` demo (allocates a multi-hundred-MB heap
        # to exercise the high-RAM support). Compiled from the tracked source by
        # the host JDK 17 to Java-17 bytecode, which the cross-built VM runs.
        HELLO_SRC="$USERLAND_DIR/apps/jdktest/Hello.java"
        HOST_JAVAC="/usr/lib/jvm/java-17-openjdk-amd64/bin/javac"
        mkdir -p "$BUILD_DIR/jdktest"
        if [ -f "$HELLO_SRC" ] && [ -x "$HOST_JAVAC" ]; then
            "$HOST_JAVAC" -d "$BUILD_DIR/jdktest" "$HELLO_SRC" || echo "[build] warn: Hello.java compile failed"
        fi
        [ -f "$BUILD_DIR/jdktest/Hello.class" ] && \
            debugfs -w "$BUILD_DIR/ext2.img" -R "write $BUILD_DIR/jdktest/Hello.class /jdk/Hello.class" > /dev/null 2>&1 || true
        echo "[build] OpenJDK java.base staged at /jdk (bin/java + lib + modules/java.base + Hello.class)"
    else
        echo "[build] JDKTEST=1 but $JDK_IMG/bin/java missing -- run 'make java.base' first"
    fi
fi
# LWJGLTEST=1: stage /lwjgl (LWJGL jars + prebuilt core/opengl natives + our
# Wayland libglfw.so + HelloWindow.class) and autostart the JVM under sway so
# LWJGL opens a GLFW window on MakaOS. Needs /jdk (staged above) + AUTOLOGIN sway.
if [ "${LWJGLTEST:-0}" = "1" ] && [ -d "$BUILD_DIR/lwjgl" ] && [ -f "$BUILD_DIR/libglfw.so" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /lwjgl" > /dev/null 2>&1 || true
    for f in lwjgl-3.3.3.jar lwjgl-glfw-3.3.3.jar lwjgl-opengl-3.3.3.jar HelloWindow.class; do
        [ -f "$BUILD_DIR/lwjgl/$f" ] && debugfs -w "$BUILD_DIR/ext2.img" -R \
            "write $BUILD_DIR/lwjgl/$f /lwjgl/$(basename "$f")" > /dev/null 2>&1 || true
    done
    # The .so's must be +x: MakaOS requires a file be executable to mmap it PROT_EXEC.
    # ext2_install_bin stages with 0100755.
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/lwjgl/natives/liblwjgl.so"        lwjgl/liblwjgl.so
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/lwjgl/natives/liblwjgl_opengl.so" lwjgl/liblwjgl_opengl.so
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libglfw.so"                       lwjgl/libglfw.so
    echo "[build] LWJGL staged at /lwjgl (jars + core natives + our libglfw.so + HelloWindow.class)"
fi
# MCTEST=1: stage a full Minecraft install under /mc (client.jar + library jars +
# extracted LWJGL natives + assets) and a writable gameDir. Our Wayland libglfw.so
# is substituted for LWJGL's bundled X11/Wayland one. Needs scripts/fetch-minecraft.sh
# to have populated build/mc/<version> first, and /jdk (staged above) + AUTOLOGIN sway.
MC_VER="${MC_VER:-1.20.4}"
MC_SRC="$BUILD_DIR/mc/$MC_VER"
if [ "${MCTEST:-0}" = "1" ] && [ -f "$MC_SRC/client.jar" ] && [ -f "$BUILD_DIR/libglfw.so" ]; then
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /mc"        > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /mc/natives" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /mc/game"   > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir /mc/game/tmp" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $MC_SRC/client.jar /mc/client.jar" > /dev/null 2>&1 || true
    # McLaunch: a thin launcher that loads net.minecraft.client.main.Main
    # reflectively and prints the full throwable chain itself -- the stock java
    # launcher masks a main-class load failure behind its localized error path.
    MCLAUNCH_SRC="$USERLAND_DIR/apps/mclaunch/McLaunch.java"
    HOST_JAVAC17="/usr/lib/jvm/java-17-openjdk-amd64/bin/javac"
    [ -x "$HOST_JAVAC17" ] || HOST_JAVAC17="$(command -v javac)"
    mkdir -p "$BUILD_DIR/mc/mclaunch"
    if [ -f "$MCLAUNCH_SRC" ] && [ -n "$HOST_JAVAC17" ]; then
        rm -rf "$BUILD_DIR/mc/mclaunch"; mkdir -p "$BUILD_DIR/mc/mclaunch"
        "$HOST_JAVAC17" --release 17 -d "$BUILD_DIR/mc/mclaunch" "$MCLAUNCH_SRC" 2>/dev/null || echo "[build] warn: McLaunch compile failed"
    fi
    # McLaunch is in package makaos.launch -> makaos/launch/McLaunch.class.
    [ -d "$BUILD_DIR/mc/mclaunch/makaos" ] && \
        ext2_install_tree "$BUILD_DIR/ext2.img" "$BUILD_DIR/mc/mclaunch/makaos" mc/makaos
    ext2_install_tree "$BUILD_DIR/ext2.img" "$MC_SRC/libraries" mc/libraries
    ext2_install_tree "$BUILD_DIR/ext2.img" "$MC_SRC/assets"    mc/assets
    # Natives must be +x (MakaOS requires +x to mmap PROT_EXEC). Stage every
    # extracted LWJGL native EXCEPT the bundled libglfw.so, then drop in ours.
    for so in "$MC_SRC/natives"/*.so; do
        base="$(basename "$so")"
        [ "$base" = "libglfw.so" ] && continue
        ext2_install_bin "$BUILD_DIR/ext2.img" "$so" "mc/natives/$base"
    done
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libglfw.so" mc/natives/libglfw.so
    echo "[build] Minecraft $MC_VER staged at /mc (client + $(ls "$MC_SRC/libraries" | wc -l) libs + natives + assets)"
fi
if [ -f "$BUILD_DIR/user_virgltest.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_virgltest.elf" bin/virgltest
    echo "[+] virgltest ELF installed at bin/virgltest (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_gltest.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_gltest.elf" bin/gltest
    echo "[+] gltest ELF installed at bin/gltest (static Mesa GL stack)"
fi
if [ -f "$BUILD_DIR/user_glfwtest.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_glfwtest.elf" bin/glfwtest
    echo "[+] glfwtest ELF installed at bin/glfwtest (GLFW-Wayland smoke test)"
fi
if [ -f "$BUILD_DIR/user_helloraw.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_helloraw.elf" bin/helloraw
fi
if [ -f "$BUILD_DIR/user_test_posix1.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_test_posix1.elf" bin/test_posix1
fi
if [ -f "$BUILD_DIR/user_unixsock_test.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_unixsock_test.elf" bin/unixsock_test
fi
if [ -f "$BUILD_DIR/user_test_io_uring.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_test_io_uring.elf" bin/test_io_uring
fi
if [ -f "$BUILD_DIR/user_smp_test.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_smp_test.elf" bin/smp_test
    echo "[+] smp_test ELF installed at bin/smp_test (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_heapalias.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_heapalias.elf" bin/heapalias
    echo "[+] heapalias ELF installed at bin/heapalias (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_makaclock.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_makaclock.elf" bin/makaclock
    echo "[+] makaclock ELF installed at bin/makaclock (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_tone.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_tone.elf" bin/tone
    [ -f "$BUILD_DIR/user_pietest.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_pietest.elf" bin/pietest
    [ -f "$BUILD_DIR/user_piehello.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_piehello.elf" bin/piehello
    [ -f "$BUILD_DIR/libdso.so" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libdso.so" lib/libdso.so
    [ -f "$BUILD_DIR/libdso2.so" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libdso2.so" lib/libdso2.so
    [ -f "$BUILD_DIR/libdsobad.so" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libdsobad.so" lib/libdsobad.so
    [ -f "$BUILD_DIR/libtlsdso.so" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libtlsdso.so" lib/libtlsdso.so
    [ -f "$BUILD_DIR/user_dltest.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_dltest.elf" bin/dltest
    # libSDL3 at its standard runtime soname /lib/libSDL3.so.0 (0755 so the loader
    # may map its text PROT_EXEC) -- the name sdl2-compat + sdltest dlopen.  (No
    # symlinks in the ext2, and two 83 MB copies won't fit, so one file, .0 name.)
    [ -f "$BUILD_DIR/libSDL3.so" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/libSDL3.so" lib/libSDL3.so.0
    [ -f "$BUILD_DIR/user_sdltest.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdltest.elf" bin/sdltest
    [ -f "$BUILD_DIR/user_sdl2test.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl2test.elf" bin/sdl2test
    [ -f "$BUILD_DIR/darkplaces_build/darkplaces.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/darkplaces_build/darkplaces.elf" bin/darkplaces
    [ -f "$BUILD_DIR/user_dplaunch.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_dplaunch.elf" bin/dplaunch
fi
if [ -f "$BUILD_DIR/user_sdl3_hello.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl3_hello.elf" bin/sdl3_hello
    [ -f "$BUILD_DIR/user_sdl3_gl.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl3_gl.elf" bin/sdl3_gl
    [ -f "$BUILD_DIR/user_sdl3_gl3.elf" ] && \
        ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl3_gl3.elf" bin/sdl3_gl3
    echo "[+] sdl3_hello ELF installed at bin/sdl3_hello (root:root 0755)"
fi
if [ -f "$BUILD_DIR/user_sdl3_audio.elf" ]; then
    ext2_install_bin "$BUILD_DIR/ext2.img" "$BUILD_DIR/user_sdl3_audio.elf" bin/sdl3_audio
    echo "[+] sdl3_audio ELF installed at bin/sdl3_audio (root:root 0755)"
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
else
    echo "[build] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "[build] !! WARNING: build/user_bash.elf is MISSING — /bin/bash AND /bin/sh"
    echo "[build] !! will NOT be installed.  /etc/passwd makes /bin/bash root's login"
    echo "[build] !! shell, so login WILL FAIL ('login: failed to exec session' loop"
    echo "[build] !! / stuck after login).  build/user_bash.elf is git-untracked since"
    echo "[build] !! b00dfe2 and built from \$BASH_SRC (default /tmp/bash-5.2), which is"
    echo "[build] !! ephemeral.  Fix: restore the prebuilt ELF from git ->"
    echo "[build] !!   git show f260b18:build/user_bash.elf > build/user_bash.elf"
    echo "[build] !! (or re-provide the bash source tree at \$BASH_SRC and rebuild)."
    echo "[build] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
fi

# ── Coreutils ────────────────────────────────────────────────────────────
for util in cat echo mkdir rm mv clear reboot uname; do
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
    # MakaOS desktop wiring: keep the swaybar `bar {}` block (taskbar);
    # point the wallpaper at the installed asset (swaybg loads it); route
    # $menu through tofi-run → swaymsg exec (no xargs on the image — sh -c
    # evaluates the $(...) at keypress time).
    # The default status_command shells out to `date` and `sleep`, which the
    # image doesn't ship (no coreutils).  A bash clock loop can't substitute:
    # this bash build has no working `printf '%()T'` (errors → exit 2) and no
    # `read -t` timeout, and it block-buffers builtin output to a pipe — so the
    # loop either exits (swaybar then renders red "[error reading from status
    # command]") or busy-fork-storms a subshell-per-tick (#GP).
    # A native helper, /bin/makaclock (userland/apps/makaclock), formats UTC
    # time, one unbuffered write(2)/s, nanosleep, never exits.  status_command
    # is wired to it.  The earlier "text status destabilizes the bar" was the
    # kernel pipe_read O_NONBLOCK bug (swaybar's getline blocked forever on the
    # status pipe) — fixed in kernel/fs/pipe.c, so a live text status is safe.
    # The explicit `font pango:DejaVu Sans Mono` below avoids the unreliable
    # generic "monospace" fontconfig match (which renders the bar text as tofu).
    # Also bind Alt+Return (Mod1) to the terminal, in addition to the default
    # Super+Return.  When MakaOS runs in a window under a host desktop, the host
    # WM globally grabs the Super/Windows key for its own launcher, so it never
    # reaches the guest and $mod+Return silently does nothing.  Alt is not
    # grabbed that way, so Alt+Return always reaches sway.  Both bindings coexist.
    sed -e 's|^include /etc/sway/config.d/\*|# include /etc/sway/config.d/* -- disabled on MakaOS (no glob in wordexp yet)|' \
        -e 's|^output \* bg .*|output * bg /usr/share/backgrounds/sway/wallpaper.png fill|' \
        -e '/bindsym .mod+Return exec .term/a\    bindsym Mod1+Return exec $term' \
        -e '/bindsym .mod+Return exec .term/a\    bindsym Mod1+s exec /bin/sdl3_hello' \
        -e 's|^set \$menu .*|set $menu swaymsg exec -- $(ls /bin \| tofi --font /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf)|' \
        -e 's|^[[:space:]]*status_command .*|    status_command /bin/makaclock|' \
        -e 's|^[[:space:]]*font .*|    font pango:DejaVu Sans Mono 11|' \
        -e '/^bar {/a\    font pango:DejaVu Sans Mono 11' \
        "$SYSROOT/etc/sway/config" > "$BUILD_DIR/etc_stage/sway_config"
    # Swiss keyboard: load a PRECOMPILED keymap directly (xkb_file) instead of
    # letting the guest's cross-built libxkbcommon walk /usr/share/X11/xkb at
    # runtime.  The RMLVO tree-walk (rules -> includes -> many files) is the
    # fragile in-guest path: the simple "us" layout compiles from names, but
    # "ch" (more includes + level3) does not, leaving sway with a NULL keymap
    # and a dead keyboard.  scripts/configs/ch.xkb is a self-contained keymap
    # compiled on the host, so in-guest sway only fopen()s and parses one file
    # (the parser already works -- it compiled "us").  Installed at
    # /etc/keymap.xkb below; applies to every keyboard.
    printf 'input type:keyboard xkb_file /etc/keymap.xkb\n' >> "$BUILD_DIR/etc_stage/sway_config"
    # Launch DarkPlaces (Quake) with Mod1+d; auto-launch it on startup when the
    # Quake data is present so the desktop boots straight into the game.
    printf 'bindsym Mod1+d exec /bin/dplaunch\n' >> "$BUILD_DIR/etc_stage/sway_config"
    # GLFW-Wayland smoke test: Mod1+g launches it; GLFWTEST=1 also autostarts it
    # so a headless run-gl.sh boot proves the window/GL-context path unattended.
    if [ -f "$BUILD_DIR/user_glfwtest.elf" ]; then
        printf 'bindsym Mod1+g exec /bin/glfwtest\n' >> "$BUILD_DIR/etc_stage/sway_config"
        [ "${GLFWTEST:-0}" = "1" ] && printf 'exec /bin/glfwtest\n' >> "$BUILD_DIR/etc_stage/sway_config"
    fi
    # LWJGL window test: autostart the JVM under sway (needs WAYLAND_DISPLAY) so
    # LWJGL -> our libglfw.so -> Mesa/virgl opens a window. -Dorg.lwjgl.glfw.libname
    # points LWJGL at our Wayland GLFW instead of its bundled X11 one.
    if [ "${LWJGLTEST:-0}" = "1" ]; then
        LWJGL_CP="/lwjgl/lwjgl-3.3.3.jar:/lwjgl/lwjgl-glfw-3.3.3.jar:/lwjgl/lwjgl-opengl-3.3.3.jar:/lwjgl"
        # -Dos.name=Linux: LWJGL's Platform.get() only knows Linux/Windows/macOS;
        # our JVM reports os.name=MakaOS, so present as Linux (we ARE linux-ABI).
        # -Dorg.lwjgl.opengl.libname=/lwjgl/libglfw.so: LWJGL resolves desktop-GL
        # entry points from the SAME .so GLFW made the context current in (one
        # Mesa/glapi instance); its glXGetProcAddress shim forwards to Mesa's
        # eglGetProcAddress, which knows the full GL 4.x set.
        printf 'exec /jdk/bin/java -Dos.name=Linux -Dorg.lwjgl.librarypath=/lwjgl -Dorg.lwjgl.glfw.libname=/lwjgl/libglfw.so -Dorg.lwjgl.opengl.libname=/lwjgl/libglfw.so -cp %s HelloWindow\n' \
            "$LWJGL_CP" >> "$BUILD_DIR/etc_stage/sway_config"
    fi
    # Minecraft: autostart the client under sway. Classpath is client.jar + every
    # library jar; natives (incl. our libglfw.so) come from /mc/natives. Offline
    # dummy credentials; gameDir + java.io.tmpdir point at the writable /mc/game.
    if [ "${MCTEST:-0}" = "1" ] && [ -f "$MC_SRC/client.jar" ]; then
        MC_CP="/mc"
        MC_CP="$MC_CP:/mc/client.jar"
        # Exclude the LWJGL *-natives-*.jar (they hold only .so's): on the classpath
        # LWJGL tries to EXTRACT them to a temp dir (which fails on MakaOS -- deep
        # mkdir + unset user.name = I/O error). Off the classpath, LWJGL loads the
        # natives directly from java.library.path=/mc/natives instead.
        for j in "$MC_SRC"/libraries/*.jar; do
            case "$(basename "$j")" in *-natives-*.jar) continue;; esac
            MC_CP="$MC_CP:/mc/libraries/$(basename "$j")"
        done
        MC_AIDX="$(sed -n 's/^assetIndex=//p' "$MC_SRC/launch.txt" 2>/dev/null)"; MC_AIDX="${MC_AIDX:-12}"
        # -XX:TieredStopAtLevel=1: run C1 only (no C2).  History: C1 originally
        #   SIGSEGV'd (LinearScan/BlockBegin) -- but that was the sys_brk remove+add
        #   heap-corruption bug trashing the C1 compiler's C-heap, NOT a C1 bug; with
        #   the brk fix (in-place heap grow) C1 runs clean.  C2 (-XX:-TieredCompilation)
        #   still DETERMINISTICALLY miscompiles java.util.HashMap.resize on MakaOS (a
        #   real C2-codegen bug, kernel clean), so cap at C1: far faster than -Xint and
        #   crash-free.  (Requires the libc sscanf %[...] scanset fix so HotSpot's -XX:
        #   parser works, AND the syscall()/futex __syscall_ret fix so WaitBarrier's
        #   FUTEX_WAIT sees -1/errno instead of a raw -EAGAIN.)
        # -Djdk.lang.Process.launchMechanism=vfork: the default POSIX_SPAWN path
        #   execs /jdk/lib/jspawnhelper, which PF-KILLs (RIP=0) on MakaOS; vfork
        #   spawns directly so oshi's hardware probes fail cleanly (IOException)
        #   instead of wedging the JVM.
        # -Djava.security.egd=file:/dev/./urandom: seed SecureRandom from the
        #   kernel CSPRNG.  Without it (and before /dev/random existed) SecureRandom
        #   fell back to the ThreadedSeedGenerator software-entropy collector, which
        #   stalled MC's bootstrap for MINUTES.  The kernel now also exposes
        #   /dev/random (== urandom), so getInstanceStrong() no longer blocks either.
        printf 'exec /jdk/bin/java -XX:TieredStopAtLevel=1 -Djava.security.egd=file:/dev/./urandom -Djdk.lang.Process.launchMechanism=vfork -Xmx2g -Dos.name=Linux -Duser.name=mc -Duser.home=/mc/game -Djava.awt.headless=true -Djava.io.tmpdir=/mc/game/tmp -Djava.library.path=/mc/natives -Djna.boot.library.path=/mc/natives -Djna.nounpack=true -Dorg.lwjgl.librarypath=/mc/natives -Dorg.lwjgl.glfw.libname=/mc/natives/libglfw.so -Dorg.lwjgl.opengl.libname=/mc/natives/libglfw.so -cp %s makaos.launch.McLaunch --username Player --version %s --gameDir /mc/game --assetsDir /mc/assets --assetIndex %s --uuid 00000000000000000000000000000000 --accessToken 0 --userType legacy --versionType release\n' \
            "$MC_CP" "$MC_VER" "$MC_AIDX" >> "$BUILD_DIR/etc_stage/sway_config"
    fi
    if [ -f "$BUILD_DIR/quake-id1/pak0.pak" ]; then
        printf 'exec /bin/dplaunch\n' >> "$BUILD_DIR/etc_stage/sway_config"
    fi
    debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir etc/sway" > /dev/null 2>&1 || true
    debugfs -w "$BUILD_DIR/ext2.img" \
        -R "write $BUILD_DIR/etc_stage/sway_config etc/sway/config" > /dev/null 2>&1 || true
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/sway        0040755 0 0
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/sway/config 0100644 0 0
    # Precompiled Swiss (ch) keymap that /etc/sway/config's xkb_file points at.
    debugfs -w "$BUILD_DIR/ext2.img" -R "write $SCRIPT_DIR/scripts/configs/ch.xkb etc/keymap.xkb" > /dev/null 2>&1 || true
    ext2_setperm "$BUILD_DIR/ext2.img" /etc/keymap.xkb 0100644 0 0
    echo "[+] sway + swaymsg installed (+ /etc/sway/config + /etc/keymap.xkb)"

    # ── Desktop environment components ────────────────────────────────
    # swaybar (taskbar) + swaynag, swaybg (wallpaper daemon), tofi
    # (launcher).  Installed only if their ports produced binaries.
    for de_bin in swaybar swaynag swaybg tofi wlr-randr; do
        if [ -f "$SYSROOT/usr/bin/$de_bin" ]; then
            ext2_install_bin "$BUILD_DIR/ext2.img" "$SYSROOT/usr/bin/$de_bin" "bin/$de_bin"
            echo "[+] DE: $de_bin installed at bin/$de_bin"
        fi
    done
    # Launcher is plain `tofi` in stdin/dmenu mode, fed by `ls /bin` from
    # the keybind ($menu).  No tofi-run/tofi-drun wrappers: the kernel ELF
    # loader has no shebang support, so a #!/bin/sh wrapper can't be exec'd
    # directly, and argv[0]-mode aliases would need 18 MB binary copies.
    # Default wallpaper → /usr/share/backgrounds/sway/wallpaper.png
    SWAY_WALL="$BUILD_DIR/third_party/sway-1.10.1/assets/Sway_Wallpaper_Blue_1920x1080.png"
    if [ -f "$SWAY_WALL" ]; then
        # debugfs mkdir does NOT create parents, and /usr + /usr/share may
        # not exist yet at this point (the fonts block below is what usually
        # creates them).  Create the WHOLE chain or the write silently fails
        # and swaybg gets a path that isn't there → black background.
        for d in usr usr/share usr/share/backgrounds usr/share/backgrounds/sway; do
            debugfs -w "$BUILD_DIR/ext2.img" -R "mkdir $d" >/dev/null 2>&1 || true
        done
        debugfs -w "$BUILD_DIR/ext2.img" \
            -R "write $SWAY_WALL usr/share/backgrounds/sway/wallpaper.png" >/dev/null 2>&1 || true
        ext2_setperm "$BUILD_DIR/ext2.img" /usr/share/backgrounds            0040755 0 0
        ext2_setperm "$BUILD_DIR/ext2.img" /usr/share/backgrounds/sway       0040755 0 0
        ext2_setperm "$BUILD_DIR/ext2.img" /usr/share/backgrounds/sway/wallpaper.png 0100644 0 0
        # VERIFY the wallpaper actually landed — a silent miss here is exactly
        # what shipped a black desktop before.  Fail loudly, don't pretend.
        if debugfs -R "stat /usr/share/backgrounds/sway/wallpaper.png" \
               "$BUILD_DIR/ext2.img" 2>/dev/null | grep -q "Size:"; then
            echo "[+] DE: wallpaper installed → /usr/share/backgrounds/sway/wallpaper.png"
        else
            echo "[!] DE: wallpaper FAILED to install — desktop will be black" >&2
        fi
    fi
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
# Single-threaded rendering.  MakaOS pthread_cond is a spin+yield
# (no kernel futex yet); foot's render worker pool can stall in the
# cond hand-off under SMP and the window never maps.  workers=0
# renders on the main thread — slower scrolling, reliable startup.
# Drop this once the kernel grows a futex.
workers=0
[tweak]
grapheme-shaping=no
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
  -m 1024M \
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
