# MakaOS

A 64-bit hobby operating system for x86-64, built from scratch — from the UEFI
bootloader up through a 4-way SMP kernel, a custom libc, and a ported
wlroots/Wayland desktop stack, all booted and tested in QEMU.

## Features

**Boot**
- UEFI/OVMF boot (`boot/uefi`): a `BOOTX64.EFI` (clang COFF) collects the E820
  memory map and GOP framebuffer, installs a higher-half mapping, and hands off
  to the kernel. A legacy second-stage C loader (`kernelLoader/`) with its own
  AHCI/PMM/VMM is kept in-tree as well.

**Kernel** (`kernel/`)
- 4-way **SMP**: per-CPU runqueues, an MLFQ scheduler over a Chase-Lev
  work-stealing deque, RCU, and lock-minimized hot paths.
- Memory: buddy-allocator PMM with per-CPU magazines, VMM with demand paging,
  copy-on-write `fork`, per-process VMAs, and a slab + kernel-heap allocator.
- Ring-3 userspace: ELF64 loader, `syscall`/`sysret` fast path, per-process
  kernel stacks via TSS, real TLS (`%fs`), futexes, and kernel-owned thread
  join — enough to run pthreads-heavy programs.
- ~118 syscalls with Linux-style `-errno` returns, including POSIX signals,
  `pipe`, `dup`/`fcntl`, `epoll`/`eventfd`/`timerfd`/`signalfd`, `io_uring`,
  and Unix-domain sockets with `SCM_RIGHTS`.
- Drivers: PCI, IRQ-driven AHCI, NVMe, virtio-gpu (DRM/KMS), virtio-net,
  virtio-input (absolute tablet), Intel HD Audio, and PS/2 keyboard + mouse.
- Networking: an ARP / IPv4 / UDP / TCP stack over virtio-net.
- Filesystems: ext2 (read **and** write) behind a small VFS, with pollable
  `/dev` character devices.

**Userspace** (`userland/`)
- A from-scratch libc (System V AMD64 ABI) with TLS, pthreads, and a `__thread`
  `errno`; **bash 5.2** runs as `/bin/sh`.
- A POSIX conformance suite (`test_posix1`, 156 assertions) plus assorted apps,
  including a DOOM port.

**Desktop** (upstream ports, not custom code)
- A wlroots-0.18 stack cross-compiled and run on the kernel's
  virtio-gpu/DRM + libinput path: the **sway** compositor, **foot** terminal,
  **swaybg** wallpaper, **tofi** launcher, and `wlr-randr`, with an absolute
  virtio-tablet pointer. Actively being stabilized — see `docs/`.

## Repository layout

```
boot/           UEFI bootloader (boot/uefi) + early boot assets
kernelLoader/   Legacy second-stage C loader (AHCI/ATA, early PMM/VMM)
kernel/         mm, proc/sched, syscalls, signals, VFS/ext2, net stack,
                drivers (AHCI / NVMe / virtio-gpu/-net/-input / HDA / PS-2)
userland/       libc, pthreads, and apps; bash as /bin/sh
scripts/        Cross-toolchain + upstream-port build scripts (port-*.sh)
docs/           Architecture, SMP design, lock inventory, session notes
build.sh        Build the sysroot and assemble the ext2 disk image
run.sh          Boot the image in QEMU (UEFI/OVMF, virtio-gpu + SDL display)
```

Engineering rules live in `PRINCIPLES.md`, `CODE_STYLE.md`, `MAKAOS.md`,
`DEBUGGING.md`, and `DO_NOT_TOUCH.md`; `CLAUDE.md` indexes them.

## Building and running

Prerequisites: the cross toolchain built by `scripts/build-toolchain.sh`, plus
`qemu-system-x86_64`, `e2fsprogs` (`mkfs.ext2`, `debugfs`), OVMF, and `nasm`.

```sh
bash build.sh   # build + assemble build/disk.img
./run.sh        # boot it in QEMU (UEFI/OVMF, SDL window)
```

QEMU exposes a GDB stub on `tcp::1234`; the kernel ELF carries DWARF, so
`gdb build/kernel.elf` then `target remote :1234` attaches.

## Status

In active development and unmistakably a learning project — but a substantial
one. It boots under QEMU (TCG and KVM), runs bash and multithreaded userland,
brings up a Wayland compositor, and drives real input/audio/network/storage
devices. Rough edges remain: the desktop's client-presentation path and a few
subsystems are still being hardened.
