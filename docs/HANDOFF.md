# MakaOS — Handoff

Start here. Read the project rules at the repo root (`PRINCIPLES.md`,
`CODE_STYLE.md`, `MAKAOS.md`, `DEBUGGING.md`, `DO_NOT_TOUCH.md`;
non-negotiable), then this file for current state. Architecture reference
is `SMP_ARCHITECTURE.md`; lock inventory is `LOCKS.md`; safe primitives are
in `PRIMITIVES.md`; the per-CPU allocator implementation notes are in
`PHASE4_REDESIGN.md` (Phase 4 is landed). (Older docs also cite a root
`CLAUDE.md`; it has not been in the repository since April 2026.)

## What MakaOS is

A from-scratch x86_64 SMP kernel booting via UEFI. C UEFI bootloader, C
kernel, userspace with bash 5.2 linked in, custom init (login + svcmgr),
ext2, virtio-net, NVMe + AHCI, full TCP/IP stack, a ported sway/wlroots
desktop on virtio-gpu (2D and virgl 3D). Competes with Linux as the design
target: performance and scalability are hard constraints.

It is a hobby OS run in QEMU. It is NOT secure and not for real use: the
June 2026 audit logged over 200 fixed memory-safety, race and privilege
bugs and still lists 33 open findings (docs/AUDIT_FINDINGS.md, status of
2026-11-08); more certainly remain, and the system as a whole has never
been security-reviewed.

## What works today — verified

### Boot path
- UEFI → BOOTX64.EFI → kernel entry → BSS clear → serial → CPU feature
  setup (NXE, PAT, SSE/SSE2, XSAVE) → early initcalls → timers → scheduler
  → subsys initcalls → login + svcmgr spawn.
- The APs (3 under the usual `-smp 4`) come online via ACPI MADT +
  real-mode trampoline; scheduler runs on every core.

### Scheduler
- MLFQ + Chase-Lev work-stealing deques (one per CPU per level).
- Steal-on-idle with random-victim + power-of-two-choices wake migration.
- Wake routing by `last_ran_cpu` for cache affinity.
- Dedicated TCP timer thread drives retransmits independently of RX
  traffic (see `kernel/net/net.c:net_tcp_timer_thread`).

### Storage
- NVMe: per-CPU I/O queue pairs, MSI-X per CPU, submit under a per-queue
  `sq_lock` (one queue per CPU, so it only guards same-CPU preemption).
  Survived 10x 4-worker stress runs in-session. Not the boot device.
- AHCI: the disk the QEMU scripts boot from (the image is attached as an
  AHCI drive). NCQ, an exclusive submit gate (`s_submit_busy`) + IRQ-safe
  `s_ci_lock`; I/O issued under a spinlock busy-polls for completion
  instead of sleeping (b1dd61a).
- ext2: bcache (4-way set-associative, 1 MiB) for metadata; pcache
  (inode-indexed, dynamic, CLOCK eviction) for file content pages.

### VFS / filesystem
- `/`, `/bin`, `/etc`, `/home`, `/tmp`, `/dev`, `/proc`, `/root`.
- Full `open/read/write/close/lseek/stat/fstat/unlink/dup/dup2/pipe`.
- `sys_mmap` supports `MAP_PRIVATE|MAP_ANONYMOUS`, `MAP_SHARED` on shmem
  fds, and **`MAP_PRIVATE` on regular file fds** (file-backed mmap,
  demand-paged through pcache, eager COW on `PROT_WRITE`). Read-only
  `MAP_SHARED` on a regular file is accepted (same path); writable
  `MAP_SHARED` on a regular file is still `-EINVAL`.
- Test coverage: `/bin/test_posix1`, the full POSIX suite: all green as of
  2026-06-23 (2281bef).

### Networking
- virtio-net 1.0 with MSI-X (with legacy-MSI fallback).
- ARP, ICMP echo, IPv4 send+recv, full TCP (SYN/SYN-ACK/ACK, retransmit,
  FIN, TIME_WAIT), UDP with ephemeral auto-bind.
- DHCP client (`/bin/net`) gets a lease from SLIRP/real DHCP and writes
  `/etc/resolv.conf`.
- Real HTTP request/response against the public internet is verified
  end-to-end (tested against `http://188.184.67.127/` — CERN Apache).

### Security mechanisms (present, not proven; see the warning above)
- `pledge`/`unveil` per task; unveil is checked on the path syscalls
  (825f782), not just `sys_open`.
- Per-fd `rights` mask: copied by dup/fork, narrowed by `restrict_fd`
  and `sendfd`, enforced at ioctl/lseek/mmap too (ea7ac2a).
- ext2 file permission bits are enforced on open (34e1c6b; before that
  every ext2 file was readable and writable by every user).
- Sessions and process groups are real: `setsid`/`setpgid`/`tcsetpgrp`
  are real syscalls, a spawned child stays in its parent's session and
  process group, and `/dev/tty` resolves to the caller's controlling
  terminal (`-ENXIO` outside a session) (3d78f74). The console is owned
  by the logged-in session (TIOCSCONSOWN, 34e1c6b).
- User pointers: `_access_ok` rejects kernel and non-canonical addresses,
  `copy_to_user`/`copy_from_user` return `-EFAULT` on an unmapped or
  faulting page via a fault-fixup table (6c36d68, e85e01b, b183a70).
- `ksec` policy daemon — see `KSEC.md` (wired but not yet the single
  policy agent).

### Userspace programs
Own apps (`userland/apps/`): `bash`, `shell`, `ls`, `ps`, `cat`, `echo`,
`mkdir`, `mv`, `rm`, `clear`, `uname`, `reboot`, `login`, `svcmgr`, `net`,
`dhcpcd`, `http_get`, `ksec`, `restrict`, `makaclock`, `doom`, the old
in-house `makadisplay` compositor / `makaterm` / `demo_client`, plus test
and demo binaries (`test_posix1`, `test_io_uring`, `test_vmalloc`,
`smp_test`, `sigtest`, `unixsock_test`, `heapalias`, `gltest`, `virgltest`,
`glfwtest`, `sdl*test` / `sdl3_*`, `piehello`, `dltest`, `jdktest`,
`lwjgltest`, `mclaunch`, ...).

Ported (via `scripts/port-*.sh`): sway 1.10.1 + swaybar/swaynag/swaymsg,
swaybg, tofi, foot, wlr-randr, dwl, tinywl, Mesa (virgl), SDL3 +
sdl2-compat, GLFW, DarkPlaces (Quake), Avian and OpenJDK 17 Zero (see
docs/DESKTOP.md, docs/JVM_PORT_PLAN.md, docs/XONOTIC_PORT.md).

## How to build and run

The canonical guide is `docs/BUILDING.md` (host packages, KVM or QEMU 9+,
the cross-toolchain bootstrap, `MINIMAL=1`). In short:
```
bash scripts/build-toolchain.sh   # once
NO_QEMU=1 bash build.sh           # without NO_QEMU it also boots QEMU headless
./run.sh                          # same image, SDL display (interactive login)
```
Booting needs x2APIC: KVM, or QEMU 9.0+ in software emulation (QEMU 8.x TCG lacks it).
`scripts/qemu-host.sh` picks the mode and the window backend for every launcher.

Serial output goes to `build/serial.txt`. `build/net.pcap` captures every
frame on the virtio-net link. `gdb -ex 'target remote :1234' build/kernel.elf`
attaches to the kernel — see "Debugging" below.

## Recent landing: the `bughunt` branch (2026-06-22 to 2026-08-13, merged 2026-11-08)

Detail lives in `docs/AUDIT_FINDINGS.md`, `docs/AUTOFIX_LOG.md`,
`docs/SESSION_2026-06-27_bughunt.md` and the plan docs named below.

- **Security/correctness audit**: rounds of bug-class scans over the
  kernel (user-pointer validation, integer overflow, UAF, races, lock
  ordering, info leaks, privilege checks), each fix verified on a booted
  image, many with a boot selftest. Shared fixes became primitives
  (`docs/PRIMITIVES.md`); a July DRY sweep removed duplicated helpers.
- **SMP**: on_cpu handshake for wake migration and work stealing (ded5751,
  a3b0d0c), pcp cross-CPU drain double-alloc (1298c11), kernel-PML4 lock
  and kstack recycling (ba5e1b0), every spinlock non-preemptible (74c8dec),
  more TLB shootdowns plus a shootdown deadlock and allocator race fix
  (b183a70; see `SMP_ARCHITECTURE.md` 9-7).
- **Fault handling**: fault-fixup table for user copies, 16-byte syscall
  stack alignment (both b183a70), `sigreturn` stack alignment (8178940).
- **Signals**: `SA_SIGINFO` from synchronous faults, per-process `sighand_t`,
  `pthread_kill`, `sigaltstack`; realtime signals 32-63 are accepted as
  no-ops (5473acc).
- **Sessions/job control and permissions**: see "Security mechanisms" above
  (34e1c6b, 3d78f74).
- **Graphics**: virtio-gpu 3D (virgl), atomic KMS, render node, static Mesa;
  wlroots/sway and SDL3 render through GLES2 on virgl
  (`docs/VIRGL_BRINGUP.md`).
- **Dynamic loading**: PIE executables and a libc `dlopen` (DT_NEEDED,
  dynamic TLS, real `libSDL3.so`) (`docs/DYNLINKER_PLAN.md`).
- **JVM**: Avian, OpenJDK 17 Zero, LWJGL over GLFW-Wayland, glibc-ABI shims
  for prebuilt `.so` files and a Minecraft launcher (c6252b3); whether
  Minecraft reaches a playable state is not recorded
  (`docs/JVM_PORT_PLAN.md`).
- **Boot**: all physical RAM identity-mapped, not just the first 1 GiB
  (856bbaa). The BIOS boot path was deleted on 2026-11-08 (ebe8a14).

## Earlier landing: 2026-06-11, the Wayland session works

Read `docs/SESSION_2026-06-11_SWAY.md` for the full account. (The
client-mapping stall described below was later resolved: the kernel futex
landed the same day (8624f18) and the June 2026 bughunt made the sway
desktop composite reliably.) Summary:

- **dwl + foot run end-to-end** (rendered terminal, live shell,
  keyboard input) — screenshot at `docs/img/foot-on-dwl-2026-06-11.png`.
- **sway 1.10.1 is ported and composites** (full default config,
  workspaces, frame commits, Mod4 bindings spawn clients).  The
  spawned client stalls before mapping its window — scheduler/wake
  lead documented in the session notes §6 (futex-less pthread_cond is
  suspect #1; a kernel futex is the next high-leverage kernel item).
- Kernel: nested-epoll readiness + wake cascade, DRM GETFB minted
  handles, MSG_DONTWAIT, /dev/ptmx + /dev/pts/N, read-only
  MAP_PRIVATE shmem, SYS_NPROC, per-task syscall kframe (signal
  delivery survives CPU migration — was a kernel panic), 1GiB sparse
  shmem, ext2 fast-path EOF clamp, Super/Meta scancodes, bounded ESP
  FAT in build.sh (the "FATAL: kernel read" boot hang).
- libc: getopt optarg fix, execl environ, thread-safe malloc,
  memfd_create, stdio ftell/fseek model fix + popen/ungetc/freopen,
  getcwd(NULL), wordexp, atexit, .init_array/.ctors in crt0 (GLib
  type system), and the whole glib/cairo/pango port surface.
- Ports: fribidi, libpng, glib, cairo, pango, sway (+ scripts/
  run-port-chain.sh to rebuild a fresh checkout end to end, and
  scripts/compositor-test.sh as the headless QMP test harness).

## Previous landing (older session's delta)

Functional fixes — all commit-worthy:
1. **bcache coherence bug** (`kernel/fs/ext2.c:bcache_fill`) — the 4-way
   cache could leave two ways tagged with the same block (one stale,
   one fresh) after a write. `bcache_fill` now invalidates any existing
   way holding the block before picking a victim. This fix alone made
   `open(O_CREAT)` persistence work across the whole VFS.
2. **ARP `skb_alloc` size** (`kernel/net/arp.c:arp_send`) — allocated
   only `sizeof(arp_pkt_t)` but then called `skb_reserve(ETH_HDR_LEN)`,
   leaving `skb_put` with no room; every ARP request silently aborted.
   Fixed to `skb_alloc(sizeof(arp_pkt_t) + ETH_HDR_LEN)`.
3. **UDP ephemeral auto-bind** (`kernel/net/socket.c:udp_auto_bind`) —
   unbound UDP sockets now get an ephemeral port registered in the UDP
   demux table on first `sendto`, so replies can find them. Previously
   every reply was dropped.
4. **TCP connect tolerant of first-send ARP miss**
   (`kernel/net/tcp.c:tcp_connect`) — SYN send can fail with -1 on an
   unresolved gateway; the PCB now stays `SYN_SENT` instead of transitioning
   straight to `CLOSED`, so the retransmit timer resends when ARP resolves.
5. **TCP SYN retransmit actually retransmits SYN**
   (`kernel/net/tcp.c:tcp_timer_tick`) — previously the retx path always
   used `TCP_ACK` flags, so a SYN retry went out as a bare ACK and got
   ignored. Now properly sends `TCP_SYN` from `SYN_SENT`, `TCP_SYN|TCP_ACK`
   from `SYN_RCVD`, data+ACK from established.
6. **Dedicated TCP timer kthread** (`kernel/net/net.c:net_tcp_timer_thread`)
   — retransmits no longer depend on incoming network traffic to tick.
7. **File-backed `mmap`** (`kernel/syscall/syscall.c:sys_mmap`) —
   `MAP_PRIVATE` on a regular fd now works. Reuses `mm_vma_add_file`, the
   existing page-fault handler does demand-paging + pcache offer +
   read-ahead + COW; the syscall is a thin shim.

Userspace:
- `http_get` gained `-e <file>` (stderr redirect) and `-r <file>` (raw
  response sidecar, appended per-hop) for scripted testing.
- `test_posix1` gained `test_mmap_file` covering the above.
- `/etc/services/net.svc` unveil changed from `rw` to `rwc` so `/bin/net`
  can `O_CREAT` `/etc/resolv.conf`.

Config:
- `build.sh` / `run.sh` audio path: `/run/user/1000/pulse/native` →
  `/mnt/wslg/PulseServer`. (Since simplified: both now use a plain
  `-audiodev pa,id=snd0`, which follows `PULSE_SERVER`; WSLg sets it.)

## What's next — recommended ordering

Picked for highest leverage per line of code, and for genuinely moving the
OS toward being a real Linux competitor.

### 1. DNS resolver (userland)
Out: `gethostbyname_ipv4` in `userland/libc/dns.c`. In: an iterative
resolver (root-hints → TLD → authoritative) with a local cache daemon,
bypass `/etc/resolv.conf` entirely. Small userland daemon + a netlink-style
socket for clients. Estimated: 1–2 days.

### 2. File-backed `MAP_SHARED` + writeback
`sys_mmap` currently rejects writable `MAP_SHARED` on regular fds
(read-only shared is accepted and served like `MAP_PRIVATE`). Add dirty-page
tracking on pcache entries, hook `msync`, and writeback on `munmap`/reclaim.
Needs a `dirty` bit on `pcache_entry` and a writeback path that locates
the originating vfs_file_t. ~200 lines. Estimated: 2–3 days.

### 3. TLS / HTTPS (userland) (partly done)
mbedTLS is ported (`scripts/port-mbedtls.sh`) and `http_get` speaks
TLS 1.3 over it (`userland/apps/http_get/https_client.c`). Missing: a
CA bundle. Without one, certificate verification fails unless the user
picks the explicit insecure mode (`MBEDTLS_SSL_VERIFY_NONE`), so HTTPS
today gives no server authentication.

### 4. Signals & job control audit (mostly done)
`test_posix1` is fully green (the SIGPIPE suite-abort was a test bug,
6e62623). Sessions, process groups, `tcsetpgrp` and the `/dev/tty`
controlling terminal are real (3d78f74), and background reads/writes get
SIGTTIN/SIGTTOU (`tty_is_background`). Left: there are only 32 signals
(NSIG=32); a realtime signal (32-63) is accepted as a no-op and never
delivered (5473acc). The proper fix is 64-bit signal masks.

### 5. poll/epoll scalability (partly done)
epoll exists (callback wakeups, hash-indexed watch table, O(1) ctl), but
once woken `epoll_wait` still scans every watched slot: there is no
per-fd ready list, so dispatch is O(watched fds). `poll` is O(n_fds) per
call. A ready list is what lets a server handle 10k connections.

### 6. ~~Phase-4 per-CPU allocators~~ — LANDED
See `PHASE4_REDESIGN.md` for full design.  `kmalloc` / `kfree` and
order-0 `pmm_buddy_alloc` / `_free` now take no locks on the hot
path; every alloc/free is a single `cmpxchg16b` on `%gs:cpu_slot`
(slab) or `%gs:pcp_hdr` (pcp).  `g_pmm_lock` demoted to depot-lock
duty (refill / drain / order ≥ 1 / refcount).  Acceptance:
99.99 % slab hit rate on 4-CPU × 50k stress (`slab_pcpu_selftest`
runs at boot only in a `MAKAOS_BOOT_SELFTESTS` build, off by default
since bbff7de; then grep `[slab_test]` in `build/serial.txt`).

## Debugging cookbook

### Attach GDB to a running kernel
```
./build.sh             # starts qemu; blocks at the end
# in another shell:
gdb -ex 'target remote :1234' build/kernel.elf
(gdb) info threads     # one per CPU
(gdb) thread apply all backtrace 5
```

### Walk the PID hash table from GDB
```
(gdb) set pagination off
(gdb) set $st = s_pid_ht
(gdb) set $i = 0
(gdb) while $i < $st->cap
  set $t = $st->slots[$i]
  if $t != 0 && $t != (task_t*)1
    printf "pid=%u ppid=%u comm='%s' state=%d ec=%d\n",
      $t->pid, $t->ppid, $t->comm, $t->state, $t->exit_code
  end
  set $i = $i + 1
end
```
Task states: `0 READY`, `1 RUNNING`, `2 DEAD`, `3 SLEEPING`, `4 ZOMBIE`.
`exit_code` of `-N` means the task was killed by signal `N`.

### Inspect disk after a boot
```
dd if=build/disk.img of=/tmp/ext2.img bs=512 skip=4096 count=1048576 status=none
debugfs -R 'ls -l /tmp' /tmp/ext2.img
debugfs -R 'cat /etc/resolv.conf' /tmp/ext2.img
```
`count` is `EXT2_SECTORS` from `build.sh` (512 MiB by default; the
JDKTEST/LWJGLTEST/MCTEST images are larger).

### Packet capture
`build.sh`'s QEMU line already has `filter-dump` writing `build/net.pcap`.
Open in Wireshark or `tshark -r build/net.pcap`.

### Live serial
```
tail -f build/serial.txt
```

## Project layout

```
kernel/
  main.c               kmain, init_kthread (spawn login + svcmgr)
  main/                subsys_init (initcall ordering)
  arch/x86_64/         entry, ISRs, syscall entry, TSS, GDT, IPIs, AP
                       trampoline + smp_boot, fault-fixup table (extable.c)
  acpi/                MADT / RSDP parsing
  time/                LAPIC, IOAPIC, PIC, PIT, TSC
  mm/                  pmm (buddy), pcp + slab_pcpu (per-CPU), vmm (PT walk),
                       mm (VMA), pcache, shmem, tlb (shootdown)
  proc/                sched, process, cpu, chaselev (work-stealing), elf,
                       signal, futex, rcu, preempt
  fs/                  vfs, ext2, dcache, pipe, proc, virtfs, fat32
  io/                  io_uring, eventfd, signalfd, timerfd
  net/                 virtio_net, eth, arp, ipv4, icmp, udp, tcp, socket,
                       unix_sock
  drivers/             storage (nvme, ahci, ata), pci, tty (+ pty), input
                       (ps/2, evdev, virtio-input), video (fb, virtio-gpu,
                       drm), audio (hda, ac97)
  security/            cred, perm, pledge, unveil, rights, ksec
  crypto/              chacha20, sha256, kernel CSPRNG
  debug/               panic, log, trace, stack walk
  syscall/             syscall dispatch (single table, by number)
  include/             cross-cutting headers (checked.h, uaccess.h, smp.h, ...)
userland/
  libc/                the custom libc (stdio, pthread, dlfcn loader, dns,
                       math, glibc-ABI compat shims, ...)
  entry/               crt entry (entry.asm)
  compat/linux/        Linux-API shims (libudev, libevdev, libseat, ...)
  apps/                own user binaries (login, svcmgr, bash, tests, ...)
  link.ld, link-pie.ld user link scripts (static, PIE)
boot/uefi/             UEFI bootloader (main.c -> BOOTX64.EFI)
scripts/               cross-toolchain + port-*.sh upstream port scripts
build.sh / run.sh      build + test + install + run
docs/                  this folder
  HANDOFF.md           you are here
  SMP_ARCHITECTURE.md  design reference (lock-minimized)
  LOCKS.md             lock inventory (incomplete; see its status note)
  PRIMITIVES.md        safe-primitive catalog
  PHASE4_REDESIGN.md   per-CPU allocator (landed)
PRINCIPLES.md, CODE_STYLE.md, MAKAOS.md, DEBUGGING.md, DO_NOT_TOUCH.md
                       project rules (scalable / performant / correct)
KSEC.md                security policy daemon protocol
```

## Rules

Read `PRINCIPLES.md`, `CODE_STYLE.md` and `MAKAOS.md` at the repo root.
Short version: performance is non-negotiable, no TODO
hacks, no O(n) in hot paths, no fixed arrays that break at Linux scale.
When in doubt, measure.
