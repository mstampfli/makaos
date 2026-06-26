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

## 3. getsockopt(SO_PEERCRED) fakes identity

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

## 4. msync / madvise / utimes / flock no-ops

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

## 5. fontconfig `fcobjshash.h` — linear scan over 55 entries

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

## 6. readv / writev via aggregation buffer

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

## 7. mknod is ENOSYS

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

## 8. posix_memalign forwards to malloc

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

## 9. `fcntl(F_SETLK / F_SETLKW / F_GETLK)` returns 0

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

## 10. mtdev — no protocol-A translation

- **What**: `userland/mtdev-stub/` is a pass-through.  `mtdev_get` forwards
  to `read(fd, evs, n*24)`; `mtdev_has_mt_event` always returns 1; no
  protocol A → B slot conversion happens.
- **Where**: `userland/mtdev-stub/mtdev.c`, generated by
  `scripts/port-mtdev.sh`.
- **Scale failure**:
  - Works today because the kernel emits MT protocol B natively on all
    input nodes (slots + tracking_id).  If we ever add a legacy protocol-A
    touchscreen driver (pre-2011 Linux devices, some custom HID tablets),
    libinput receives unslotted finger events and mis-tracks contacts.
- **Target**:
  - Drop in upstream mtdev's ~500 LOC protocol-A→B converter when we first
    import a protocol-A-only driver.  The ABI stays identical; only the
    stub body swaps.  No libinput rebuild needed.
- **Blocking order**:
  - Independent.  Triggered by the first protocol-A input driver.

---

## 11. libinput CLI tools not built

- **What**: `port-libinput.sh` builds only `libinput.a`; the tool
  executables (`libinput-debug-events`, `libinput-record`,
  `libinput-analyze`, `libinput-measure`, `libinput-quirks`,
  `libinput-test`) are skipped.
- **Where**: `scripts/port-libinput.sh` (`build_install` uses
  `ninja libinput.a` instead of the default target).
- **Scale failure**:
  - No on-device input debugging.  `libinput-debug-events` is how everyone
    diagnoses "why isn't my touchpad tap working" in the real world.
    Without it, regressions in our evdev driver are opaque to operators.
- **Target**:
  - Add the libc surface they need: `scanf`/`fscanf` that read from stdin
    with real format parsing (today we have `vsscanf` only), `uname`
    exported from `libc.a` as a real symbol (today it is `static inline` in
    `libc.h` and invisible to sysroot consumers), `tmpfile` exercised +
    hardened, and `localtime_r` already landed.
- **Blocking order**:
  - libc additions only.  No kernel work.  Do when the first compositor
    regression forces manual input debugging.

---

## 12. udev_device_get_parent chain is shallow

- **What**: Only DRM devices synthesize a PCI parent (`0000:00:02.0`
  hardcode).  Every other synthetic device returns NULL for
  `udev_device_get_parent()`.
- **Where**: `scripts/port-libudev.sh` →
  `udev_device_get_parent_with_subsystem_devtype`.
- **Scale failure**:
  - libinput's quirk engine (`quirks.c`) walks `device->parent` chains to
    match by PCI / USB vendor:product.  Without the chain, hardware-specific
    quirks (ThinkPad TrackPoint acceleration, Apple trackpad tuning) never
    apply — input feels generic, regardless of device.
  - Likewise, `udev_prop()` fallback through the parent chain returns no
    NAME / ID_INPUT_* properties for input devices, so libinput classifies
    every device as "unknown type."
- **Target**:
  - Real bus topology: kernel exposes a device registry (item 2) that
    reports each device's parent (PCI slot, USB port, platform bus).
    libudev's `get_parent` walks that tree in userspace.
- **Blocking order**:
  1. Kernel: device registry (item 2).
  2. libudev: rewrite `get_parent` as a registry query.

---

## 13. gcc specs override for shared-library links

- **What**: `scripts/makaos-gcc-specs` patches `*startfile:` so `crt0.o`
  is skipped for `-shared` links.  Wired into
  `scripts/makaos-meson-cross.ini` via `c_link_args`/`cpp_link_args`.
- **Where**: `scripts/makaos-gcc-specs`, `scripts/makaos-meson-cross.ini`.
- **Scale failure**:
  - Not a scale issue, a toolchain correctness issue.  The cross-toolchain
    config sets `STARTFILE_SPEC = "crt0.o%s"` unconditionally instead of
    `%{!shared: crt0.o%s}`.  Every shared-library port must remember to
    inherit this specs file; meson ports get it automatically via the
    cross-file, but hand-rolled builds that bypass meson will miss it.
- **Target**:
  - Fix the cross-toolchain source config at build time: edit
    `gcc/config/i386/makaos.h` (or wherever the spec is defined) to use
    `%{!shared: crt0.o%s}`, rebuild the toolchain, delete the specs file +
    cross-file references.
- **Blocking order**:
  - Independent.  Do at next toolchain rebuild cycle.

---

## Guarantee

No entry in this ledger may move to "deferred forever."  Each is tied to a
tier or milestone:

- **Pre-Hyprland-launch (Tier 6)**: item 3 (real SO_PEERCRED).  Compositors
  need it to authenticate Wayland clients.  (The previous per-device evdev
  entry resolved 2026-04-21 when the multi-device input_device_t kernel
  rewrite landed — that's why the list skips from 2 to 3.)
- **Pre-SDL3/curl (Tier 7)**: items 4, 9 (msync, flock, fcntl record
  locks).  SDL and curl use these for file I/O correctness.
- **Pre-Ladybird (Tier 8)**: items 1, 2, 12 (libseat with revoke, libudev
  with hot-plug, real parent-chain topology).  Ladybird spawns many child
  processes with distinct UIDs per SECURITY_V2.md; input-heavy apps hit
  the parent-chain limitation.
- **Independent / opportunistic**: items 5, 6, 7, 8, 10, 11, 13 — fix
  when a concrete port or benchmark forces it.

Each stub carries a `// TODO(scalability-debt-ledger-#N)` comment pointing
back to its entry here.  When the entry is resolved, both the comment and
the ledger entry go.

## virtio-input: single-device binding
`kernel/drivers/input/virtio_input.c` binds only the FIRST virtio-input
PCI function (today's topology: one `virtio-tablet-pci`).  Scalable
replacement: loop the PCI scan, hold per-device state (queues, evdev
handle, MSI-X vector per device) in a kmalloc'd array — mechanical, no
design change.  evdev/devfs/udev-stub rows for event3+ go in
`kernel/fs/virtfs.c` + `scripts/port-libudev.sh` alongside.

## Boot self-tests: latent kthread-exit corruption — RESOLVED (2026-06-22)
The boot battery (chaselev/slab/typesafe/... in kernel/main.c) is behind
`#ifdef MAKAOS_BOOT_SELFTESTS` (off by default to keep normal boot fast; enable
with `SELFTESTS=1 bash build.sh`).  It used to intermittently DEADLOCK: a
self-test worker kthread exited with a RUNTIME-corrupted `cleartid_addr`
(e.g. 0x53004300530000, non-canonical) → `task_notify_cleartid`'s
`copy_to_user` #GP → that CPU wedged in the fault handler → a peer's
`synchronize_rcu` spun forever.

ROOT CAUSE (found): the PMM per-CPU page cache `pcp_drain_all()` claimed a
remote CPU's stash with plain reads/writes while the owner popped via
cmpxchg16b, so the same physical frame was handed to two owners — one of them
the task slab, which is how a kthread's `cleartid_addr` got scribbled with
another allocation's bytes.  FIXED in `mm/pcp` (commit 809127d): read-then-
cmpxchg16b-claim drain (see `cmpxchg16b_abs`).  The `_access_ok`/user-copy
hardening (iter2-3) had already made the resulting #GP non-fatal; this fix
removes the corruption itself.

VERIFIED 2026-06-22 (`SELFTESTS=1 bash build.sh`, headless boot): the FULL
battery PASSES — chaselev (0 duplicates), slab_test (200k allocs, hit rate
9999/10000, no crash), pmm10-test (all 8 order=10 allocs DISTINCT, the
double-alloc detector), typesafe/dcache/io_uring/eventfd/timerfd/socketpair/
scm_rights/signalfd/drm-mock — all PASS, with no `[pmm] DOUBLE-ALLOC`, no #GP,
no RCU stall.  Safe to enable by default if the boot-time cost is acceptable.

---

## execve in a multithreaded process: full POSIX leader-switch not done (F97)

F97 closed the cross-domain page-table UAF/LPE where a multithreaded exec freed
the old PML4 hierarchy while sibling THREAD_SHARE_MM threads still ran on it.
The fix (kernel/syscall/syscall.c, sys_exec): when g_current is NOT the sole
owner of its task_mm_t (refs > 1), it SIGKILLs the thread group, detaches
g_current onto a FRESH task_mm_t, and drops its ref on the old shared mm -- so
old_pml4 is freed by task_mm_release only when the last killed sibling exits
(no spin/deadlock, no UAF).  This is SECURITY-COMPLETE (the LPE is closed).

REMAINING (correctness, not safety): the full POSIX de_thread leader-switch is
NOT implemented.  g_current's tgid/leader identity is left unchanged.  For the
common case (the group LEADER calls execve) this is already correct -- g_current
stays its group's sole survivor with tgid == pid.  For the rare case where a
NON-leader thread calls execve, g_current keeps a tgid pointing at the (now
SIGKILL'd, soon-reaped) old leader, instead of becoming the new leader and
taking over the leader pid.  That is a benign residual (no UAF; the surviving
thread runs the new image), but POSIX would have the exec'ing thread assume the
leader identity.  The proper fix needs the tgid-index re-key + child-list /
waitpid reparenting (a de_thread).  This path is INERT at boot (every boot exec
is single-threaded, refs == 1), so it is untestable by the boot battery; the
[exec_mm] selftest covers the sole-owner-vs-shared decision threshold.

---

## pty: reopen /dev/pts/N after slave close while master open returns NULL (F104)

F104 closed the reopen-after-slave-close use-after-free: pty_slave_close now
clears pty->slave_file (and slave_claimed) when the master is still open, and
pty_open_slave_by_index uses vfs_tryget instead of a raw refcount bump.  So a
reopen of /dev/pts/N after the slave was fully closed (while the master stays
open) now returns NULL (a clean ENXIO-style failure) instead of resurrecting
the freed slave vfs_file_t.

REMAINING (functionality, not safety -- the UAF is fully closed): Linux devpts
lets a process reopen /dev/pts/N while the master is alive and get a fresh
slave handle.  MakaOS now rejects that reopen.  This does NOT affect the normal
terminal flow (the shell opens its slave once and keeps it open; dup/fork share
the same file via the refcount and an open-while-already-open succeeds through
vfs_tryget -- only a reopen AFTER a full close is rejected).  The proper fix is
to BUILD A FRESH slave vfs_file_t on demand in pty_open_slave_by_index when
slave_file==NULL && master_open (decoupling the slave file's lifetime from
per-open): extract a pty_make_slave_file(pty) helper used by both pty_alloc and
the reopen path, and set pty->slave_file before the s_pty_head insert in
pty_alloc so slave_file==NULL unambiguously means "closed" (no not-fully-built
window).  Deferred to keep this fix surgical on the boot-critical pty path; the
[pty_reopen] selftest pins the safety invariant (slave_file cleared, no UAF).

---

## io_uring: non-SQPOLL concurrent enter() can double-execute a SQE (F105)

F105 closed the io_uring SQ-consumer memory-safety bugs: enter() no longer
consumes a SQPOLL ring (the poller is the sole consumer), and
io_wq_ensure_worker is serialised so at most one async worker is ever spawned
(closing the orphaned-worker use-after-free for every consumer-race path).

REMAINING (correctness, NOT memory-safety -- no UAF/OOB): on a NON-SQPOLL ring,
two threads that share the ring fd and both call io_uring_enter concurrently
still run the SQ consumer loop unserialised (sys_io_uring_enter takes only
fdget/fdput), so they can read the same sq_head, dispatch the same SQE twice
(a double send/write/open) and double-advance the head.  This is io_uring
MISUSE (the ring is designed for a single submitter thread) and yields only a
duplicate operation + duplicate CQE, never memory corruption (the worker-orphan
UAF is already closed).  The proper fix mirrors Linux's uring_lock over
io_submit_sqes but WITHOUT holding a spinlock across a blocking sync dispatch
(IORING_OP_WRITE etc. block on disk I/O): claim one SQE under a per-ring
submit_lock (COPY it to a local to avoid the user-reuses-the-slot TOCTOU,
advance + publish sq_head), release the lock, then dispatch the copy outside it;
the IO_LINK chain logic must claim the whole chain under the lock.  Deferred to
keep F105 surgical on the boot-critical io_uring path; the memory-safety bugs
are fully closed.
