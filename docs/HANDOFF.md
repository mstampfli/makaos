# MakaOS — Handoff

Start here. Read `CLAUDE.md` for project rules (non-negotiable), then this
file for current state. Architecture reference is `SMP_ARCHITECTURE.md`;
lock inventory is `LOCKS.md`; the per-CPU allocator implementation
notes are in `PHASE4_REDESIGN.md` (Phase 4 is landed).

## What MakaOS is

A from-scratch x86_64 SMP kernel booting via UEFI. NASM bootloader, C
kernel, userspace with bash 5.2 linked in, custom init (login + svcmgr),
ext2, virtio-net, NVMe + AHCI, full TCP/IP stack. Competes with Linux as
the design target — performance and scalability are hard constraints.

## What works today — verified

### Boot path
- UEFI → NASM bootloader → kernel entry → BSS clear → serial → CPU feature
  setup (NXE, PAT, SSE/SSE2, XSAVE) → early initcalls → timers → scheduler
  → subsys initcalls → login + svcmgr spawn.
- All 4 APs come online via ACPI MADT + real-mode trampoline; scheduler
  runs on every core.

### Scheduler
- MLFQ + Chase-Lev work-stealing deques (one per CPU per level).
- Steal-on-idle with random-victim + power-of-two-choices wake migration.
- Wake routing by `last_ran_cpu` for cache affinity.
- Dedicated TCP timer thread drives retransmits independently of RX
  traffic (see `kernel/net/net.c:net_tcp_timer_thread`).

### Storage
- NVMe: per-CPU I/O queue pairs, MSI-X per CPU, lock-free submit,
  NCQ-style completion. Survived 10x 4-worker stress runs in-session.
- AHCI: NCQ submit lock + IRQ-safe ci_lock, handled as the fallback.
- ext2: bcache (4-way set-associative, 1 MiB) for metadata; pcache
  (inode-indexed, dynamic, CLOCK eviction) for file content pages.

### VFS / filesystem
- `/`, `/bin`, `/etc`, `/home`, `/tmp`, `/dev`, `/proc`, `/root`.
- Full `open/read/write/close/lseek/stat/fstat/unlink/dup/dup2/pipe`.
- `sys_mmap` supports `MAP_PRIVATE|MAP_ANONYMOUS`, `MAP_SHARED` on shmem
  fds, and **`MAP_PRIVATE` on regular file fds** (file-backed mmap,
  demand-paged through pcache, eager COW on `PROT_WRITE`).
- Test coverage: `/bin/test_posix1` — full POSIX suite. Works.

### Networking
- virtio-net 1.0 with MSI-X (with legacy-MSI fallback).
- ARP, ICMP echo, IPv4 send+recv, full TCP (SYN/SYN-ACK/ACK, retransmit,
  FIN, TIME_WAIT), UDP with ephemeral auto-bind.
- DHCP client (`/bin/net`) gets a lease from SLIRP/real DHCP and writes
  `/etc/resolv.conf`.
- Real HTTP request/response against the public internet is verified
  end-to-end (tested against `http://188.184.67.127/` — CERN Apache).

### Security
- `pledge`/`unveil` per task; enforced in `sys_open` and other syscalls.
- Per-fd `rights` mask, restricted on `dup`/`fexec`.
- `ksec` policy daemon — see `KSEC.md` (wired but not yet the single
  policy agent).

### Userspace programs
`bash`, `shell`, `ls`, `ps`, `cat`, `echo`, `mkdir`, `mv`, `rm`, `clear`,
`reboot`, `login`, `svcmgr`, `net`, `dhcpcd`, `http_get`, `ksec`,
`restrict`, `test_posix1`, `test_vmalloc`, `smp_test`, `tone`, `home`,
`hello`, `helloraw`, `makadisplay` (compositor), `makaterm` (terminal
emulator), `doom` (DOOM shareware, rendered via makadisplay), `demo_client`.

## How to build and run

Prereqs on the host (WSL2 Ubuntu here, but any modern Linux works):
- `gcc`, `clang`, `ld.lld`, `nasm`, `objcopy`, `sgdisk`, `mtools`,
  `e2fsprogs`, `qemu-system-x86` (8.x tested).
- KVM module loaded (`modprobe kvm_amd` or `kvm_intel`); `/dev/kvm`
  readable by user's `kvm` group.
- WSLg pulseaudio (`/mnt/wslg/PulseServer`) if on WSL — otherwise point
  `-audiodev pa,...,server=` at a reachable pulse socket in `build.sh` /
  `run.sh`.
- (Optional) put GNU bash 5.2 source at `/tmp/bash-5.2` to build `bash`.

Build, smoke-test, and install the disk image:
```
./build.sh         # compiles, installs binaries into ext2, boots QEMU headless
./run.sh           # same image, SDL display (interactive login)
```

Serial output goes to `build/serial.txt`. `build/net.pcap` captures every
frame on the virtio-net link. `gdb -ex 'target remote :1234' build/kernel.elf`
attaches to the kernel — see "Debugging" below.

## Recent landing — 2026-06-11: the Wayland session works

Read `docs/SESSION_2026-06-11_SWAY.md` for the full account.  Summary:

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
  `/mnt/wslg/PulseServer`. If you're not on WSL, point it at your real
  pulse socket.

## What's next — recommended ordering

Picked for highest leverage per line of code, and for genuinely moving the
OS toward being a real Linux competitor.

### 1. DNS resolver (userland)
Out: `gethostbyname_ipv4` in `userland/libc/dns.c`. In: an iterative
resolver (root-hints → TLD → authoritative) with a local cache daemon,
bypass `/etc/resolv.conf` entirely. Small userland daemon + a netlink-style
socket for clients. Estimated: 1–2 days.

### 2. File-backed `MAP_SHARED` + writeback
`sys_mmap` currently rejects `MAP_SHARED` on regular fds. Add dirty-page
tracking on pcache entries, hook `msync`, and writeback on `munmap`/reclaim.
Needs a `dirty` bit on `pcache_entry` and a writeback path that locates
the originating vfs_file_t. ~200 lines. Estimated: 2–3 days.

### 3. TLS / HTTPS (userland)
Ship a minimal TLS 1.2/1.3 client library. You already have `ksec` and
entropy (`/dev/urandom`). Needed: AES-GCM, ChaCha20-Poly1305, SHA-256,
HMAC, HKDF, X.509 parser + CA bundle, TLS state machine. Big — easily
a week. But once done, `http_get https://...` just works because the only
protocol change is the transport layer below `GET /`.

### 4. Signals & job control audit
`test_pipe` in `test_posix1` kills itself with uninstalled SIGPIPE; real
shell pipelines need SIG_IGN SIGPIPE defaults and proper job control
(tcgetpgrp/tcsetpgrp, SIGTTIN/SIGTTOU). Bash runs today but some features
are cosmetic.

### 5. poll/epoll scalability
Current `poll` is O(n_fds) per call. epoll with a ready-list (O(1)
dispatch) is what lets a server handle 10k connections. Prereq for any
serious network service.

### 6. ~~Phase-4 per-CPU allocators~~ — LANDED
See `PHASE4_REDESIGN.md` for full design.  `kmalloc` / `kfree` and
order-0 `pmm_buddy_alloc` / `_free` now take no locks on the hot
path; every alloc/free is a single `cmpxchg16b` on `%gs:cpu_slot`
(slab) or `%gs:pcp_hdr` (pcp).  `g_pmm_lock` demoted to depot-lock
duty (refill / drain / order ≥ 1 / refcount).  Acceptance:
99.99 % slab hit rate on 4-CPU × 50k stress (`slab_pcpu_selftest`
runs at boot — grep `[slab_test]` in `build/serial.txt`).

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
dd if=build/disk.img of=/tmp/ext2.img bs=512 skip=4096 count=65536 status=none
debugfs -R 'ls -l /tmp' /tmp/ext2.img
debugfs -R 'cat /etc/resolv.conf' /tmp/ext2.img
```

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
  arch/x86_64/         entry, ISRs, syscall entry, TSS, GDT, APIC
  mm/                  pmm (buddy), vmm (PT walk), mm (VMA), pcache
  proc/                sched, process, cpu, chaselev (work-stealing)
  fs/                  vfs, ext2, pipe, proc, virtfs
  net/                 virtio_net, eth, arp, ipv4, icmp, udp, tcp, socket
  drivers/             storage (nvme, ahci), pci, tty, input, video, audio
  security/            acl, pledge, unveil, rights, ksec-bridge
  syscall/             syscall dispatch (single table, by number)
  include/             cross-cutting headers
userland/
  libc/                entry, libc, stdio, dns, math, setjmp
  apps/                every user binary (login, svcmgr, bash, etc.)
  link.ld              user link script
boot/                  UEFI stage (BOOTX64.EFI source)
build.sh / run.sh      build + test + install + run
docs/                  this folder
  HANDOFF.md           you are here
  SMP_ARCHITECTURE.md  design reference (lock-minimized)
  LOCKS.md             every lock + its justification
  PHASE4_REDESIGN.md   per-CPU allocator (landed)
CLAUDE.md              project rules (scalable / performant / correct)
KSEC.md                security policy daemon protocol
```

## Rules

Read `CLAUDE.md`. Short version: performance is non-negotiable, no TODO
hacks, no O(n) in hot paths, no fixed arrays that break at Linux scale.
When in doubt, measure.
