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
> - **Accounts are a toy:** passwords are stored in plaintext (`root` / `toor`).
>   File permissions are checked (owner/group/other bits on open, create, delete and
>   path walk), but only the modes the image was built with exist: `chmod`/`chown`
>   report success and store nothing, and every new file is `0644`, owned by its
>   creator.
> - **Many syscalls fake success**, and the pledge sandbox allows anything it doesn't
>   list.
> - **Known holes are open:** the security audit
>   ([docs/AUDIT_FINDINGS.md](docs/AUDIT_FINDINGS.md)) is old and outdated (June and
>   July 2026, partial), and even it lists 33 open findings, including sandbox escapes
>   and local denial of service. Expect many more it does not list.
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
- **Ported software** (optional, `bash scripts/setup.sh --desktop`): sway, foot, swaybg,
  tofi, Mesa (virgl), SDL2/3, GLFW, DarkPlaces (Quake), OpenJDK 17 (Zero), Avian, LWJGL.

## Building and running

On Ubuntu/Debian or WSL2:

```sh
git clone https://github.com/mstampfli/makaos.git && cd makaos
bash scripts/setup.sh          # cross compiler + Base image (about 8 min)
./run.sh                       # boot; log in as root / toor
```

`setup.sh` checks the host packages and prints the `apt install` line for anything
missing. Booting needs x2APIC: KVM, or QEMU 9.0 or newer. Without KVM (for example WSL2
without nested virtualization), run `bash scripts/setup.sh --qemu` once; it builds QEMU
9.2 and `run.sh` picks it up. `--desktop` also builds sway, Mesa, SDL and the JVMs.

Everything else is in **[docs/BUILDING.md](docs/BUILDING.md)**; start with its
**[At a glance](docs/BUILDING.md#at-a-glance)** tables:

- the three image sizes (Minimal, Base, Desktop) and how to build each;
- every start script: window or not, 2D or 3D, whether sway starts, and whether files
  you create in MakaOS are kept;
- **every `build.sh` run wipes the disk image's file system.**

### Your own programs and files

MakaOS has a text editor, `kilo` (Ctrl-S save, Ctrl-Q quit). Easier: write and compile
on your machine, then copy into the image while MakaOS is shut down:

```sh
mkdir -p /tmp/t
toolchain/bin/x86_64-pc-makaos-gcc --sysroot=build/sysroot -O2 -o /tmp/t/prog prog.c
bash scripts/diskfiles.sh put /tmp/t        # -> /root/t in MakaOS
bash scripts/diskfiles.sh get /root/out.txt /tmp/from-makaos
```

Files you create in MakaOS are kept across `./run.sh` boots, but **not across
`build.sh`** (copy them in again). Details:
[Getting files in and out](docs/BUILDING.md#getting-files-in-and-out).

### For security research

- Use the Base image (`setup.sh` without `--desktop`): it has the same kernel as the
  Desktop image and rebuilds in about 25 s. The GPU driver and its ioctls are in every
  image; the 3D (virgl) ioctls need the 3D device: `bash scripts/run-gl-gui.sh` (writes
  are discarded there).
- Where to look: open findings in
  [docs/AUDIT_FINDINGS.md](docs/AUDIT_FINDINGS.md), the setuid/policy agent in
  [KSEC.md](KSEC.md), file permission checks in `kernel/security/perm.c`, syscalls in
  `kernel/syscall/syscall.c`, the GPU driver in `kernel/drivers/video/drm.c` and
  `virtio_gpu.c`. Accounts: `root` / `toor`, unprivileged `maka` / `maka`.
- Debugging: QEMU's GDB stub on `:1234` (`gdb build/kernel.elf`, `target remote :1234`)
  and the kernel log on serial; see [Debugging](docs/BUILDING.md#debugging).

## Repository layout

```
boot/uefi/      UEFI bootloader (BOOTX64.EFI)
kernel/         arch, mm, proc (scheduler, signals), syscall, fs, net, drivers,
                acpi, time, io, security, crypto, debug
userland/       libc, apps (one directory per program), compat/linux shims, entry stub
scripts/        setup.sh (one-command setup), diskfiles.sh (files in/out), build-qemu.sh,
                qemu-host.sh (KVM or QEMU 9 picker), cross toolchain, port-*.sh, launchers
docs/           build guide, feature inventory, design docs, audit and session logs
build.sh        builds everything and assembles build/disk.img (wipes its file system)
build_bash.sh   cross-builds bash 5.2 (called by build.sh)
run.sh          boots the image in a QEMU window (KVM, or QEMU 9 software emulation)
```

Documentation (all of it):

- [docs/BUILDING.md](docs/BUILDING.md): build, run, files, debugging, troubleshooting
- [docs/FEATURES.md](docs/FEATURES.md): what works, with evidence and status
- [docs/AUDIT_FINDINGS.md](docs/AUDIT_FINDINGS.md): security and concurrency findings,
  open and fixed
- [KSEC.md](KSEC.md): the setuid/policy agent
- [DEBUGGING.md](DEBUGGING.md): the kernel's logging, panic, tracing and stack-walk
  facilities
- [docs/LOCKS.md](docs/LOCKS.md) and [docs/SMP_ARCHITECTURE.md](docs/SMP_ARCHITECTURE.md):
  which lock protects what, and how scheduling, RCU and SMP are built

Older plans, session logs and contributor rules were removed on 2026-11-09; they are in
the git history (for example `git show 6e92687:docs/HANDOFF.md`), and some code comments
still name them.

## Status

In active development, and a learning project first. Checked on 2026-11-09 from a fresh
clone, without KVM: login and bash, the sway desktop on the GPU with foot, SDL3 and GLFW
windows, Avian and OpenJDK. Known problems:

- DarkPlaces (Quake) under sway renders briefly, then goes black or is disconnected.
- sway sometimes takes one to two minutes to start instead of a few seconds (cause not
  yet found).
- More in the troubleshooting table of [docs/BUILDING.md](docs/BUILDING.md#troubleshooting)
  and the "Known not working or missing" section of
  [docs/FEATURES.md](docs/FEATURES.md).
