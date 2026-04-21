# MakaOS Scalability Debt Ledger

Running log of every stub, hardcode, and fixed-size artifact introduced for
short-term progress.  For each: what's wrong at Linux-scale, and the concrete
path from "stubbed now" to "scalable later."

This is a commitment, not a wishlist.  When we ship multi-user / USB hot-plug /
multi-GPU / real session switching, every entry here must be resolved.  New
stubs added to this repo MUST be appended to this file in the same commit —
no silent shortcuts.

Format per entry:
- **What**: the stub / hardcode / fixed array.
- **Where**: file:line pointer (or file glob).
- **Scale failure**: what breaks, at what scale.
- **Target**: the correct design.
- **Blocking order**: what must be built first (kernel / libc / library) to
  unblock the real implementation.

---

## 1. libseat stub (always-grant, fixed 64-device table)

- **What**: `libseat_open_device` tracks FDs in `fd_table[64]`; `enable_seat`
  fires immediately; no revoke/session-switch semantics.
- **Where**: `userland/libseat/libseat.c`.
- **Scale failure**:
  - Breaks at 65 simultaneously-open seat devices (compositor with 64+ input
    devices — not uncommon on USB-heavy setups).
  - Breaks under any multi-session scenario: fast-user-switch, remote desktop,
    kiosk mode, locked screen with handoff.
  - No revoke path — device grabs cannot be withdrawn when a privileged
    process requests them (breaks the capability-first security model in
    `SECURITY_V2.md`).
- **Target**:
  1. `fd_table` becomes a dynamically-grown radix tree or hash keyed by
     device_id. O(1) ops, no fixed ceiling.
  2. Seat lifecycle driven by the kernel capability broker (ksec v2 from
     SECURITY_V2.md): seats become capability contexts, activation/deactivation
     is a capability transition, revoke is a capability revocation that fires
     `disable_seat` immediately.
  3. `libseat_switch_session` becomes real — kernel session table lookup,
     capability transfer, DRM master handoff.
- **Blocking order**:
  1. Kernel: capability broker (ksec v2) per SECURITY_V2.md — Tier-gated on
     "stable-kernel milestone."
  2. Kernel: session table subsystem (new).
  3. libseat: rewrite as broker client.
  4. Compositor: integrate `disable_seat`/`enable_seat` for real session swaps.

---

## 2. libudev replacement (planned) — static device list

- **What**: Native libudev equivalent will return a hardcoded list of
  `/dev/input/event0` and `/dev/dri/card0`.
- **Where**: `userland/libudev/` (to be written).
- **Scale failure**:
  - USB keyboard / mouse plugged in: invisible. No hot-plug detection.
  - Multiple GPUs (iGPU + dGPU laptop, multi-GPU workstation): only card0
    seen.
  - Touchpads, tablets, gamepads: unknown.
- **Target**:
  1. Kernel: device registry subsystem (all registered drivers report name +
     class + IDs). New ioctl interface: `/dev/makadev` control fd with
     `MAKA_DEV_LIST`, `MAKA_DEV_QUERY`, `MAKA_DEV_SUBSCRIBE` opcodes.
  2. Kernel: hot-plug events via io_uring completions on the subscribe fd.
  3. libudev: thin client of `/dev/makadev`. Enumerate via `MAKA_DEV_LIST`,
     resolve sysattrs via `MAKA_DEV_QUERY`, watch changes via the subscribe fd.
- **Blocking order**:
  1. Kernel: central device registry (every driver's register path plugs in).
  2. Kernel: `/dev/makadev` control fd.
  3. Kernel: hot-plug event stream.
  4. libudev: rewrite as registry client.

---

## 3. Kernel evdev — single unified input node

- **What**: `/dev/input/event0` serves both keyboard and mouse events from
  one ring buffer.
- **Where**: `kernel/drivers/input/evdev.{c,h}`.
- **Scale failure**:
  - Cannot attribute events to specific devices. libinput-class logic (palm
    rejection, per-device calibration) needs to know "this motion came from
    device X."
  - No way to open exclusively one device (e.g. game wants just the gamepad).
  - Blocks clean multi-device future (USB hot-plug adds devices with their
    own capability bitmaps).
- **Target**:
  - One `/dev/input/eventN` node per physical device. event0 = PS/2
    keyboard, event1 = PS/2 mouse, event2..N = USB devices as they enumerate.
  - Each node has its own ring, its own grab counter, its own capability
    metadata (exposed via ioctl).
  - `libc` enumerator scans `/dev/input/event*` at startup + subscribes to
    device registry events for new additions.
- **Blocking order**:
  1. Kernel: per-device ring allocation in input_core.
  2. Kernel: device registry (same subsystem as item 2).
  3. Kernel: per-device ioctls for capability reporting.

---

## 4. getsockopt(SO_PEERCRED) fakes identity

- **What**: Returns `pid=getpid(), uid=0, gid=0` for any AF_UNIX socket.
- **Where**: `userland/libc/sys_socket.c:getsockopt`.
- **Scale failure**:
  - Wayland compositor cannot authenticate clients — every process appears
    to be root.
  - Blocks per-app UID from SECURITY_V2.md (each app gets its own UID,
    compositor rejects wrong-UID socket connections).
  - Privilege-sensitive IPC (D-Bus resolver daemon, mentioned in SECURITY_V2)
    has no way to verify caller identity.
- **Target**:
  1. Kernel: track peer credentials on every AF_UNIX `connect` / `accept`.
     Store `(pid, uid, gid, cap_ctx)` in the unix_sock struct.
  2. Kernel: new `SYS_GETSOCKOPT` syscall dispatching `SO_PEERCRED` et al.
  3. libc: real getsockopt routes through the syscall.
- **Blocking order**:
  1. Kernel: unix_sock struct gains peer-cred fields.
  2. Kernel: SYS_GETSOCKOPT.
  3. libc: replace fake with real syscall.

---

## 5. msync / madvise / utimes / flock no-ops

- **What**: All four return 0 without doing work.
- **Where**: `userland/libc/libc.c` (msync, madvise); `userland/libc/sys_time.c`
  (utimes, lutimes); `userland/libc/sys_file.c` (flock).
- **Scale failure**:
  - `msync`: file-backed writable mmap has no way to ensure reaches disk.
    Breaks databases (sqlite, lmdb) that depend on msync for durability.
  - `madvise`: the kernel has no pressure hints — cannot reclaim evictable
    pages under memory pressure. Scale: multi-GB working sets.
  - `utimes`: `rsync`, `make`, `tar` all touch timestamps. Without real
    updates, build systems get confused by the epoch timestamps.
  - `flock`: cross-process file locks silently succeed — sqlite/lmdb/gdbm
    corrupt their DBs when concurrent writers appear.
- **Target**:
  - Kernel: SYS_MSYNC dispatching to the page-cache writeback path.
  - Kernel: SYS_MADVISE storing hints in the vma table; reclaimer honours.
  - Kernel: SYS_UTIMES updating inode atime/mtime through virtfs.
  - Kernel: SYS_FLOCK backed by an inode-keyed lock table (shared-mode uses
    a counter, exclusive-mode uses a waitqueue).
- **Blocking order**:
  1. Kernel: page-cache with writeback (required for msync anyway).
  2. Kernel: per-inode attribute storage with timestamps in the fs layer.
  3. Kernel: flock table subsystem (can use existing futex/waitqueue infra).
  4. libc: wrappers become real syscalls.

---

## 6. fontconfig `fcobjshash.h` — linear scan over 55 entries

- **What**: Replaced gperf's perfect hash with a linear `strncmp` loop.
- **Where**: generated at port time in `scripts/port-fontconfig.sh`.
- **Scale failure**:
  - O(n=55) per lookup. Called once per property during config-parse and
    once per font during query. At 10,000 fonts × 30 props = 300K lookups
    on cold cache. Not a hot path on any realistic UI, but measurable on
    cache populate.
- **Target**:
  - Install `gperf` on the host, let fontconfig's build use the real
    perfect hash.  OR: write a MakaOS-side perfect-hash generator (awk /
    python) that matches gperf's output format.
- **Blocking order**:
  - Independent.  Install gperf whenever.

---

## 7. readv / writev via aggregation buffer

- **What**: libc `readv` and `writev` allocate (stack or heap) a contiguous
  buffer, do a single read/write, then scatter back.
- **Where**: `userland/libc/unistd.c`.
- **Scale failure**:
  - Extra memcpy per I/O — at 10 Gbit/s networking that's a real CPU cost.
  - Breaks atomicity guarantees for non-stream fds (wayland happens to be
    stream, so we're ok there).
  - Heap allocation on the hot path for > 4 KiB chunks.
- **Target**:
  - Kernel: `SYS_READV` / `SYS_WRITEV` — iovec passed through directly,
    kernel loops over elements atomically where the underlying fd supports
    it (AF_UNIX: yes; stream TCP: yes; datagram: batched).
  - libc: plain one-syscall wrapper.
- **Blocking order**:
  1. Kernel: SYS_READV / SYS_WRITEV.
  2. libc: replace aggregator with direct syscall.

---

## 8. mknod is ENOSYS

- **What**: `mknod(path, mode, dev)` returns -1 / ENOSYS.
- **Where**: `userland/libc/sys_stat.c`.
- **Scale failure**:
  - Any port expecting to create device nodes at runtime (libudev_zero's
    create-missing-node path, custom init systems) fails.  We mitigate
    because our `/dev/*` is virtfs-populated at boot.
- **Target**:
  - Kernel: SYS_MKNOD wiring virtfs to create a node with the given
    major/minor.  Needs a device-driver registry (same infra as item 2).
- **Blocking order**:
  1. Kernel: device registry (item 2).
  2. Kernel: virtfs gains write-mode for mknod.
  3. libc: wrapper routes through SYS_MKNOD.

---

## 9. posix_memalign forwards to malloc

- **What**: Ignores requested alignment; returns whatever malloc gives.
- **Where**: `userland/libc/stdlib.c`.
- **Scale failure**:
  - libdrm / pixman / ffmpeg request 16, 32, 64-byte alignment — our malloc
    happens to return 16-aligned, so we're OK up to alignment=16.  Breaks
    at alignment=32+ (e.g. AVX-512 code requests 64).
- **Target**:
  - Rewrite malloc to track alignment per-block, or add a separate aligned
    allocator with power-of-two alignment up to page size.  Keep O(1) on
    common sizes.
- **Blocking order**:
  - Independent.  Do when the first port requests alignment > 16.

---

## 10. `fcntl(F_SETLK / F_SETLKW / F_GETLK)` returns 0

- **What**: POSIX record locks silently succeed without kernel backing.
- **Where**: `userland/libc/fcntl.c`.
- **Scale failure**:
  - Same as flock (item 5) but for byte-range locks — sqlite, postgres,
    bdb all corrupt their DBs under concurrent writers.
- **Target**:
  - Kernel: inode-keyed byte-range lock table.  Same subsystem as flock
    (item 5), just finer-grained.
- **Blocking order**:
  - Part of item 5's flock table work.  One lock table, two client APIs.

---

## Guarantee

No entry in this ledger may move to "deferred forever."  Each is tied to a
tier or milestone:

- **Pre-Hyprland-launch (Tier 6)**: items 3, 4 (per-device evdev nodes +
  real SO_PEERCRED).  Hyprland relies on multi-device input and on
  authenticating compositor clients.
- **Pre-SDL3/curl (Tier 7)**: items 5, 10 (msync, flock, fcntl record locks).
  SDL and curl use these for file I/O correctness.
- **Pre-Ladybird (Tier 8)**: items 1, 2 (libseat with revoke, libudev with
  hot-plug).  Ladybird spawns many child processes with distinct UIDs per
  SECURITY_V2.md.
- **Independent / opportunistic**: items 6, 7, 8, 9 — fix when a concrete
  port or benchmark forces it.

Each stub carries a `// TODO(scalability-debt-ledger-#N)` comment pointing
back to its entry here.  When the entry is resolved, both the comment and
the ledger entry go.
