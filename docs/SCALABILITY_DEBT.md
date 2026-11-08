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

---

## DRM: GEM handles are monotonic (never recycled), so the res3d array can grow with churn

Phase 2c unified each drm_file's GEM handle namespace onto one c->next_handle
counter (dumb + res3d) so a handle names exactly one object.  The counter is
monotonic: a create-then-destroy cycle never reuses a handle number.  Two
consequences, both bounded and neither a correctness bug:

1. find_res3d indexes an array by handle-1.  Because res3d handles now interleave
   with dumb handles (a render fd that PRIME-imports N dumbs then makes a res3d
   gets a res3d handle ~N), the array can be sparser than the live res3d count --
   res3d_cap tracks the max res3d HANDLE, not the live count.  For real workloads
   (Mesa render fd: mostly res3d + a handful of PRIME imports; compositor card
   fd: mostly dumbs + ~0 res3d) this is a few KB at most.

2. A long-lived fd that churns many res3d over its lifetime grows res3d_cap
   without bound (handles keep climbing).  This predates 2c -- the old
   next_res3d_handle was already monotonic.

The scalable fix is Linux's model: a single per-fd handle allocator with
recycling (idr / small hash keyed by handle) mapping handle -> {type, object},
replacing BOTH the dumb linked list and the res3d array.  Recycling needs a
generation tag to keep a stale userspace handle from aliasing a new object.
Deferred: handles are per-fd and bounded by that fd's lifetime allocation count;
no real client hits this, and the O(1) res3d lookup + unified-namespace
correctness are both preserved today.

## ext2/AHCI: filesystem metadata I/O runs under fs spinlocks via busy-polling

ext2's allocation and writeback paths hold spinlocks across block I/O:
alloc_inode / alloc_block hold the per-group s_group_locks[g] across the bitmap
read + bitmap/bgd write; the inode writeback holds the per-inode seqlock
write_lock across the 128-byte inode write.  This predates the scheduler change
that made spin_lock disable preemption (74c8dec): the code's comments call the
write_lock "preemptible" and were written when a writer could sleep inside
write_block.  After 74c8dec, sched_sleep panics with preempt_depth > 0, so the
AHCI IRQ-driven path (which sleeps on slot/submit/completion wait queues) can no
longer be called under those spinlocks -- open(O_CREAT) silently halted the CPU.

Fix in place (ahci.c AHCI_WAIT): the AHCI slot-alloc, submit-busy and completion
waits busy-poll (re-running ahci_rescan_completions each spin) when
preempt_depth > 0 instead of sched_sleep-ing.  This is correct -- the ISR still
fires with IRQs enabled and the rescan services completions directly -- but it
holds the fs spinlock AND disables preemption on that CPU for the whole I/O.  On
fast media (QEMU: microseconds) this is fine; on slow media a metadata write
under a group lock stalls the holding CPU and any CPU spinning on that group
lock for the I/O duration (~ms).

The scalable fix is to keep block I/O OUT of the fs allocation/inode spinlocks:
do the in-memory allocation decision under the lock (pin the bitmap/bgd/inode
blocks in bcache, set the bit / stage the inode in memory), release the lock,
then persist to disk with the normal sleeping path outside the lock.  Concurrent
allocators stay consistent via the pinned bcache block.  Deferred: the poll path
is correct and unblocks the whole Wayland-compositor bring-up (every compositor
creates a socket lockfile); no MakaOS workload issues enough concurrent
metadata writes for the lock-hold latency to matter yet.

---

## 14. virtio-gpu resource backing is physically contiguous (single mem_entry) -- RESOLVED (2026-07-11)

**RESOLVED.** DRM/virgl resource backing is now scatter-gather, not contiguous.
Every dumb, res3d, fb, PRIME clone and cursor backs its pages with a
`shmem_create_dma` object -- scattered (best-effort large blocks, degrading to
order-0, never requiring a contiguous span), pinned (DMA-safe + swap-immune),
fully resident -- and attaches to the device as a multi-entry mem_entry list
(`virtio_gpu_resource_attach_backing_sg`, chained-descriptor transport for any
run count).  Landed in: `shmem_create_dma` (d2a0467), the DRM conversion
(aba707a), and the cap/budget rework (this entry).  What changed vs the debt:

- **Fragmentation cliff: gone.** No resource needs a high-order contiguous block;
  a large buffer allocates as scattered pages even when RAM is fragmented.
- **Power-of-2 waste: gone.** `bytes_alloc` is now exact page-rounded, not
  rounded up to the next power-of-2 buddy order.
- **64 MiB per-resource cap: REMOVED.** It was purely a contiguity guard-rail (a
  buddy alloc that big fails under fragmentation); with scatter-gather there is no
  contiguous block to fail.  The only upper bound now is the backing store's
  capacity (`SHMEM_MAX_PAGES`, which also keeps `bytes` within uint32); the
  pinned-memory budget is the real guard.
- **Per-task 256 MiB magic cap: REPLACED** with two RAM-derived ceilings in
  `drm_charge` -- GLOBAL <= 3/4 of physical RAM (system-wide, keeps GPU memory
  from starving the kernel) and PER-TASK <= 1/2 of RAM (fairness), both floored so
  a small VM still runs a real GL workload, both enforced BEFORE any page is
  pinned so exceeding either is a clean `-ENOMEM`, never a crash.
- **STILL PLANNED (not a blocker):** a privileged raise-the-limit knob
  (sysctl/boot-param or ioctl) + per-client override, so a trusted compositor /
  GPU app that legitimately needs more can be granted it while the gate stays for
  untrusted clients (design captured in memory `project_makaos_gpu_pin_budget`).
  Also optional: 2 MiB huge-page userspace mmap for large runs (the allocator
  already clusters into 2 MiB-capable blocks; the mmap path currently installs
  4 KiB PTEs, zero-fault).

Verified headless (SELFTESTS=1): `shmem-dma` populate/pin + >64 MiB allocate,
`drm-budget` (>64 MiB charge OK + runaway reject + counters restored), the
virtio-gpu SG multi-entry round-trip, and the drm-mock vtable battery all PASS.

The original debt writeup, for history:

- **What**: every virgl/DRM resource (textures, render targets, window/scanout
  backing, vertex/staging buffers) is allocated with `pmm_buddy_alloc(order)` --
  a physically contiguous, power-of-2 block -- and attached to the device as a
  single `nr_entries=1` mem_entry via `virtio_gpu_resource_attach_backing_single`.
  The two memory ceilings the driver enforces are BOTH consequences of this one
  decision, not independent policy:
    - the **64 MiB per-resource cap** (`size64 > (64u << 20)` in
      `drm_ioctl_virtgpu_resource_create`), and
    - the **per-task charge ceiling** (`drm_per_task_limit()` in `drm_charge`,
      now `max(512 MiB, RAM/2)` -- was a fixed 256 MiB magic number).
  Neither ceiling reflects a device or hardware limit; each exists only to keep a
  client from asking the buddy allocator for a contiguous block large enough to
  exhaust or fragment physical RAM.
- **Where**: `kernel/drivers/video/drm.c` (`drm_ioctl_create_dumb`,
  `drm_ioctl_virtgpu_resource_create`, `drm_charge`/`drm_per_task_limit`),
  `kernel/drivers/video/virtio_gpu.c`
  (`virtio_gpu_resource_attach_backing_single`).
- **Scale failure**:
  - **Fragmentation cliff.** A 1080p BGRA window is ~8 MiB = order-11 = 2048
    contiguous pages. Fine at boot; after hours of churn high-order buddy blocks
    get scarce, so `pmm_buddy_alloc` returns ENOMEM even with plenty of free
    *scattered* memory. Windows are large and long-lived -- exactly the
    allocations most likely to fail.
  - **Power-of-2 internal waste.** A 5 MiB texture rounds up to an 8 MiB block:
    up to ~2x waste on every non-power-of-2 resource, and windows are rarely a
    clean power of two.
  - **Both memory caps are symptoms of the contiguity, not real limits.** The
    64 MiB per-resource cap and the per-task ceiling exist because high-order
    contiguous allocs are dangerous (fragmentation, ENOMEM-with-free-RAM), not
    because any resource or task needs them. A leak in the per-task charge is
    what crashed DarkPlaces in ~13 s (fixed in 34ad96b); making the ceiling
    RAM-proportional instead of a fixed 256 MiB reduces the blast radius, but
    the caps only truly *dissolve* under scatter-gather: scattered pages don't
    fragment the buddy allocator, so there is nothing to guard against and both
    numbers can go away.
  - Neither the guest nor the device requires physical contiguity: virtio-gpu's
    RESOURCE_ATTACH_BACKING is *built* for scatter-gather (an array of
    nr_entries {addr,length} mem_entries). Using one entry manufactures a
    contiguity requirement the hardware never imposes. What actually needs to be
    contiguous is *virtual* (the guest's single mmap of the resource + the HHDM
    kernel view), which page tables provide over scattered frames. Physical
    contiguity buys only simple pointer math.
- **Target**: allocate per-page (or a few large blocks), describe the backing to
  the device as a multi-entry scatter list (nr_entries > 1), map it
  virtually-contiguous into the userspace VMA and into a kernel window. Removes
  the fragmentation cliff, the power-of-2 waste, AND both artificial memory caps
  (per-resource 64 MiB and per-task) in one move -- once backing is scattered
  there is no contiguity to protect, so the ceilings can be dropped or raised to
  a pure fairness policy. Mirrors Linux GEM/GBM sg-list backing.
- **Blocking order**: needs (1) `virtio_gpu_resource_attach_backing` variant that
  emits an nr_entries>1 mem_entry array from a page list; (2) an mmap resolver
  (`drm_resolve_dumb_mmap`) that installs PTEs for a page list rather than one
  contiguous range; (3) a kernel virtually-contiguous mapping (vmap-style) for
  the HHDM-style readback path. Correctness is unaffected today (contiguous is a
  strict subset of what the device accepts); this is purely a scaling stopgap.
- **Note**: NOT the cause of the 2026-07 black-texture bug -- that backing is
  contiguous, correctly sized, and correctly attached; the 16x16 round-trip
  selftest proves the mechanism. Contiguity is a scaling problem, not a
  correctness one here.

---

## 15. libiconv is an identity-only stub (UTF-8/ASCII passthrough)

- **What**: the ported `iconv` (`userland/compat/linux/libiconv/iconv.c`, formerly
  heredoc'd in `scripts/port-libiconv.sh`) accepts only UTF-8/ASCII aliases and
  does a bounded `memcpy`; `iconv_open` returns EINVAL for any real charset.  No
  transcoding.
- **Where**: `userland/compat/linux/libiconv/`.
- **Scale failure**: any GLib/gettext consumer converting a non-UTF-8 charset
  (legacy locale data, some font/text pipelines, file names in other encodings)
  gets EINVAL or wrong bytes.  Fine while everything is UTF-8; breaks the moment a
  real conversion is needed.
- **Target**: a real charset-conversion primitive -- port GNU libiconv, or a
  shared MakaOS converter table -- exposed as the general Linux `iconv` surface.
- **Blocking order**: independent; do when a port needs non-UTF-8 conversion.

## 16. libdrm device enumeration is a synthesized single device

- **What**: `scripts/port-libdrm.sh` Python-patches `drmGetDevice2` /
  `drmGetDevices2` / `drmGetDeviceFromDevId` to return ONE hardcoded virtio-gpu
  device (PCI `0x1af4:0x1050`, bus `0000:00:04.0`, nodes card0 + renderD128)
  instead of enumerating real devices.
- **Where**: `scripts/port-libdrm.sh` (the `makaos_synth_virtio_device` patch).
- **Scale failure**: multi-GPU (iGPU + dGPU), a different PCI slot, or any
  non-virtio DRM device is invisible; the PCI IDs are wrong for real hardware.
  Mesa/libdrm see exactly one fixed device.
- **Target**: real enumeration via the **same kernel device registry as #2
  (libudev)** -- both want "list the devices + their bus topology + IDs".  libdrm
  becomes a thin client of that registry.
- **Blocking order**: kernel device registry (shared with #2), then rewrite the
  three enumerators as registry queries instead of a synth patch.

## 17. mprotect is a no-op stub (no SYS_MPROTECT)

- **What**: `userland/libc/jdk_compat.c`'s `mprotect()` returns success without
  changing any page permissions -- MakaOS has no user-facing mprotect syscall.
- **Where**: `userland/libc/jdk_compat.c`; the VMM already has the perm-tightening
  + TLB-shootdown machinery (see `kernel/mm/tlb.h`), just no syscall entry.
- **Scale failure**: guard pages (stack-overflow detection), W^X for JITs, and any
  runtime that relies on `PROT_NONE`/`PROT_READ` transitions silently keep their
  original permissions. The JVM's StackOverflowError guard page is inert.
- **Target**: add `SYS_MPROTECT` wired into the VMM (change VMA perms + shoot down
  the TLB), then make libc `mprotect` a thin syscall wrapper.
- **Blocking order**: independent; do when a runtime needs real page protection.

## 18. Only 32 signals; realtime signals (32-63) are accepted-but-undeliverable

- **What**: `NSIG` is 32 and the per-task `pending`/`blocked` masks are `uint32_t`,
  so signals 32-63 (the POSIX realtime range `SIGRTMIN..SIGRTMAX`) cannot be
  delivered. `sys_sigaction`/`sys_kill` accept them as benign no-ops so glibc-style
  callers do not treat the failure as fatal (see `SIGRTMAX_COMPAT`).
- **Where**: `kernel/proc/signal.h` (`NSIG`, `sigstate_t.pending/blocked`),
  `kernel/syscall/syscall.c` (`sys_sigaction`, `sys_kill`).
- **Scale failure**: RT signals never fire. OpenJDK's `sun.nio.ch.NativeThread`
  installs its blocking-I/O interrupt on `SIGRTMAX-2`, so `Thread.interrupt()` of a
  thread blocked in a native NIO call is a no-op; any app using `SIGRTMIN+n` for
  queued/realtime notification gets nothing.
- **Target**: widen the signal masks to `uint64_t` and `NSIG` to 64, then deliver
  32-63 like standard signals (every `1u << (sig-1)` becomes `1ull << ...`).
- **Blocking order**: independent; touches the signal-delivery core, so pair with
  a sigtest extension that exercises a signal >= 32.

## 19. Mesa/GL is whole-archived per-consumer instead of shared .so's

- **What**: `libglfw.so` (the LWJGL/Minecraft window+GL library) whole-archives
  the entire Mesa stack (libEGL/libGLESv2/libglapi/libgbm + the virgl DRI
  megadriver) into one self-contained shared object. Every future GL consumer
  that needs its own dlopen-able GL would re-embed the same ~15 MB of Mesa.
- **Where**: `build.sh` (the `libglfw.so` link), `scripts/port-mesa.sh`
  (`-Ddefault_library=static`, `-Dglx=disabled`, `-Dglvnd=false`).
- **Why it is this way (not just laziness)**: two real constraints.
  (a) *Single-instance correctness*: GLFW makes the GL context current in one
  `libglapi` state; LWJGL's GL calls must dispatch through that SAME state or every
  `gl*` hits a NULL context. One self-contained .so trivially guarantees one
  instance. (b) *Duplicate globals*: Mesa's EGL frontend and the DRI megadriver
  each define the same global (e.g. `dri2_lookup_egl_image_validated`), resolved
  today by `-Wl,--allow-multiple-definition` at static-link time. Split into
  separate .so's, those become genuine cross-.so ODR conflicts the loader could
  bind to the wrong copy.
- **Scale failure**: N GL apps -> N embedded Mesa copies on disk and N rebuilds;
  no shared GL upgrade path.
- **Target**: build Mesa as real shared objects (`libEGL.so`, `libglapi.so`,
  `libgbm.so`, the DRI driver), deduped by SONAME by our loader (already done for
  the JVM), so GLFW and every GL consumer share ONE Mesa instance the clean way.
  First untangle the duplicate-global symbols (give the megadriver its own copies
  or hidden visibility) so `--allow-multiple-definition` is no longer needed.
- **Blocking order**: independent of the Minecraft bring-up (inside one process
  the single self-contained .so is correct); do it before a second GL app ships.
