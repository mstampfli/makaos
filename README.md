# MakaOS

> [!CAUTION]
> ## MakaOS is a hobby OS. It is NOT usable, and it is NOT secure.
>
> MakaOS is a learning project. It is not an operating system you can use for anything
> real, and it is deliberately not hardened. Run it only inside QEMU, and never give it
> a network or data you care about.
>
> - **No exploit mitigations worth the name:** no ASLR, no KASLR, no SMAP, no kernel
>   W^X, and `mprotect` silently does nothing.
> - **Accounts are a toy:** passwords are stored in plaintext (`root` / `toor`), and
>   `chmod`/`chown` don't store anything.
> - **Many syscalls fake success**, and the pledge sandbox allows anything it doesn't
>   list.
> - **Known holes are open:** the security audit (`docs/AUDIT_FINDINGS.md`) still lists
>   33 open findings, including sandbox escapes and local denial of service.
> - **It is not Linux:** its syscall ABI is its own, so Linux programs don't run on it
>   directly.
>
> Nothing here comes with any guarantee. Treat every "works" in this repository as "worked
> in QEMU on the author's machine".

A 64-bit hobby operating system for x86-64, written from scratch: a UEFI bootloader, an
SMP kernel, a custom libc, and its own userland (about 101,000 lines of C and assembly),
plus ported software such as bash, a sway/wlroots desktop, Mesa, SDL and OpenJDK.

**[docs/FEATURES.md](docs/FEATURES.md)** lists everything it can do, kernel internals
included, each item with its evidence and an honest status (verified, implemented,
partial, stub, not built, missing).

## Highlights

- **Boot:** UEFI only (`boot/uefi`): a clang-built `BOOTX64.EFI` hands the memory map,
  framebuffer and ACPI tables to a higher-half kernel.
- **Kernel:** SMP (4 CPUs in QEMU) with per-CPU run queues on Chase-Lev work-stealing
  deques, RCU, a buddy + per-CPU + slab allocator stack, demand paging and copy-on-write
  `fork`, real TLS, futexes, signals with `SA_SIGINFO`, sessions and job control, and a
  dynamic loader (static PIE + `dlopen`). 119 syscalls in its own numbering, including
  `epoll`, `io_uring`, `eventfd`/`timerfd`/`signalfd`, and Unix sockets with
  `SCM_RIGHTS`.
- **Drivers:** PCI, AHCI, NVMe, virtio-gpu (DRM/KMS, virgl 3D), virtio-net,
  virtio-input, Intel HDA, PS/2.
- **Networking:** ARP / IPv4 / UDP / TCP / DHCP / DNS, plus TLS through mbedTLS.
- **Filesystems:** ext2 (read and write) behind a VFS with dentry and inode caches.
- **Userland:** a from-scratch libc with pthreads; bash 5.2 as `/bin/sh`.
- **Ported software** (built separately, see `docs/DESKTOP.md`): sway, foot, swaybg,
  tofi, Mesa (virgl), SDL2/3, GLFW, DarkPlaces (Quake), OpenJDK 17 (Zero), Avian, LWJGL.

## Building and running

Full guide: **[docs/BUILDING.md](docs/BUILDING.md)**. Booting needs x2APIC: either KVM,
or QEMU 9.0 or newer in software emulation (slower). Building needs neither.

Quickest path, kernel + login + shell, no cross-compiler (under a minute):

```sh
MINIMAL=1 NO_QEMU=1 bash build.sh
./run.sh
```

Full build:

```sh
bash scripts/build-toolchain.sh   # once, builds the x86_64-pc-makaos cross-compiler
NO_QEMU=1 bash build.sh
./run.sh
```

Log in as `root` / `toor`.

## Repository layout

```
boot/uefi/      UEFI bootloader (BOOTX64.EFI)
kernel/         arch, mm, proc (scheduler, signals), syscall, fs, net, drivers,
                acpi, time, io, security, crypto, debug
userland/       libc, apps (one directory per program), compat/linux shims, entry stub
scripts/        cross-toolchain, port scripts (port-*.sh), run helpers
docs/           build guide, feature inventory, design docs, audit and session logs
build.sh        builds everything and assembles build/disk.img
build_bash.sh   cross-builds bash 5.2 (called by build.sh)
run.sh          boots the image in QEMU (KVM, OVMF, SDL window)
```

Engineering rules: `PRINCIPLES.md`, `CODE_STYLE.md`, `MAKAOS.md`, `DEBUGGING.md` and
`DO_NOT_TOUCH.md`.

## Status

In active development, and a learning project first. It has booted in QEMU with KVM to a
login prompt, run bash and multithreaded programs, brought up the sway desktop, and run
Java and Quake. Large parts are partial or missing: see the "Known not working or
missing" section of `docs/FEATURES.md`.
