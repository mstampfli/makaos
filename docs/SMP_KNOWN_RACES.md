# SMP Known Races — Tracking

Every race / unsafe shared-state access that is **currently harmless on
UP** but **will be a bug the instant APs boot**.  Every entry lists
which phase is scheduled to fix it.  Phase 9 (AP bring-up) will not be
allowed to proceed until this list is empty.

Format:
- **Location** — file:symbol
- **Race** — what's unsafe
- **Why safe on UP** — why it doesn't matter today
- **Fix phase** — which phase is responsible
- **Fix strategy** — what the phase will do

---

## 0. PMM / kheap all internal state — RESOLVED (Phase 4 bridge)

- **Location:** `kernel/mm/pmm.c`, `kernel/mm/kheap.c`
- **What was unsafe:** every buddy free list, slab cache head, per-frame
  refcount, per-frame pincount, slab tracker array. Zero synchronization
  before Phase 4.
- **Fix:** global `g_pmm_lock` spinlock (IRQ-safe) wraps every public
  PMM mutating entry point. Read-only queries stay lock-free on
  aligned loads. See `docs/PHASE4_REDESIGN.md` for the per-CPU
  magazine plan that eventually replaces this bottleneck.
- **Status:** SMP-correct, but single-lock contention under multi-CPU
  load. Acceptable bridge until Phase 4 proper lands.

---

## 1. `irq_wait` waiter list head — RESOLVED (Phase 3)

- **Location:** `kernel/arch/x86_64/irq_wait.c`
- **Fix:** `s_head[irq]` linked list replaced with
  `s_wq[irq]` of `wait_queue_t`. Waiters stack-allocate a
  `task_we_t` and push onto the queue via the lock-free MPSC CAS.
  `irq_notify` calls `wait_queue_wake_all` which detaches the whole
  chain with one xchg.  Same-CPU races covered by `cli`; cross-CPU
  races covered by the atomic primitives in the wait queue itself.

---

## 2. Scheduler wait/run-queue lists — RESOLVED (Phase 5)

- **Fix:** all of `s_heads[]`, `s_tails[]`, `s_sleep_head`,
  `s_zombie_head`, and `s_reschedule` are gone.  Their contents moved
  into `cpu_t.rq` (a `cpu_rq_t` struct with per-CPU run queues, sleep
  list, and zombie list) and `cpu_t.reschedule_pending`.  Every access
  goes through the owning CPU's `rq_lock` (IRQ-safe spinlock).
- **Cross-CPU wake:** `sched_wake(t)` takes `g_cpus[t->home_cpu].rq_lock`
  directly and enqueues on the target CPU's run queue.  Sets
  `reschedule_pending` on the target.  TODO Phase 9: if target CPU is
  idle/halted, send IPI — currently relies on the next timer tick.
- **Single lock, SMP-safe.**  On one CPU the lock is always
  uncontended (`lock cmpxchg` = ~20 cycles).  Under SMP the lock
  serializes only the few ops that touch the target CPU's rq, which
  is by construction a small fraction of scheduler work (most is
  enqueue_on / dequeue_local on the running CPU).

---

## 3. `task_idx` (pgid/tgid/sid) hash tables — RESOLVED (Phase 6)

- **Location:** `kernel/proc/sched.c` — `s_pgid_ht`, `s_tgid_ht`, `s_sid_ht`
- **Fix:** each `task_idx_t` now carries its own IRQ-safe spinlock.
  All insert/remove and `task_idx_*_walk` operations acquire that lock
  for the duration; writers hold it across slot mutation, walkers hold
  it across the full callback invocation so concurrent insert/remove
  cannot tear the doubly-linked bucket list.  `signal_send_group` and
  `signal_send_pgrp` now use the new `task_idx_tgid_walk` /
  `task_idx_pgid_walk` APIs, which are O(threads-in-bucket).
- **Why spinlock and not full RCU:** signal delivery is not on the
  per-packet hot path; the bucket walks are microsecond-scale and the
  lock is uncontended in practice.  The RCU-COW machinery used by the
  network tables would add complexity with no measurable win here.

---

## 4. Zombie list — RESOLVED (Phase 5)

- **Fix:** each CPU has its own `cpu_t.rq.zombie_head`.  When a task
  exits, `sched_add_zombie` puts it on ITS home CPU's zombie list
  under that CPU's `rq_lock`.  `sched_reap_*` walks every CPU's
  zombie list (also under the per-CPU lock) to find a match.  Under
  SMP this means the reaper does `num_cpus` lock acquisitions in
  the worst case — acceptable because wait()/waitpid() are cold path.

---

## 5. `sched_wake` cross-CPU preemption flag — RESOLVED (Phase 5)

- **Fix:** the flag lives in `cpu_t.reschedule_pending`.  `sched_wake`
  sets it on the TARGET CPU's cpu_t, under that CPU's rq_lock,
  after enqueuing the woken task on that CPU's run queue.
- **TODO Phase 9:** if target is halted in idle, send an IPI so the
  target CPU context-switches immediately instead of waiting for its
  next tick.

---

## 6. Hash-table writers — RESOLVED (Phase 6)

All of these are now lock-free on the read path via QSBR RCU.  The
whole table (not individual slots) is wrapped in a heap-allocated
object whose pointer is published via `rcu_assign_pointer`.  Writers
serialise on a small writer spinlock, build a fresh table via
copy-on-write, publish, and `call_rcu` the old table.  Readers do a
preempt-disable + plain pointer load + hash walk — zero atomics, zero
cache-line bouncing, per-packet cost is a single memory load.

- `kernel/net/arp.c` — `s_arp` (was `s_cache`).  `arp_lookup` (called
  on every outgoing packet) is lock-free.
- `kernel/net/socket.c` — `s_udp` (was `s_udp_slots`).  `udp_table_find`
  (called per UDP packet in `socket_deliver_udp`) is lock-free and
  wrapped in `rcu_read_lock()` that also covers the RX enqueue, so
  the socket cannot be freed under the delivery path.  `socket_t`
  teardown is `call_rcu`-deferred via `sock_free_rcu`.
- `kernel/net/unix_sock.c` — `s_unix_ns` (was an open-addressing array).
  `unix_sock_close` defers the full teardown via `call_rcu`, so
  concurrent `connect`/`sendto` readers inside `rcu_read_lock()` see a
  consistent sock until they exit the reader section.
- `kernel/mm/shmem.c` — `s_namespace`.  `shmem_t.refcount` is now
  atomic; `shmem_ns_find` bumps the refcount via `shmem_tryget` inside
  `rcu_read_lock()` so the object cannot be freed under the caller.
  Final teardown runs from `shmem_free_rcu` after a grace period.
- `kernel/net/tcp.c` — `s_pcb_head` linked list.  `tcp_recv` and
  `tcp_timer_tick` walk under `rcu_read_lock()`; `tcp_pcb_free` unlinks
  then `call_rcu`s the pcb destruction.

See LOCKS.md entries for exact writer-lock / call_rcu usage.

---

## 7. fd table reads vs. mutation — RESOLVED (Phase 8A)

- **Fix:** `task_files_t` now holds a pointer to a separately-allocated
  `fdtable_t { vfs_file_t** fd_table; uint32_t* fd_flags; uint32_t cap }`
  published via `rcu_assign_pointer` and loaded on the read side with
  `__atomic_load_n(..., ACQUIRE)`.  `fd_table_grow` allocates a fresh
  `fdtable_t`, copies the live slots, installs the new pointer, and
  hands the old `fdtable_t` to `call_rcu(fdtable_free_rcu, old)` so
  any in-flight reader that already loaded the old pointer stays
  valid until the grace period ends.

  `vfs_file_t.refcount` is now atomic (`__atomic_fetch_add/sub`),
  with a new `vfs_tryget` (CAS-if-nonzero) so a reader that observes
  a slot can bump the refcount safely even against a concurrent last-
  close.  The hot lookup is a new helper `fdget(fd)` which does
  `rcu_read_lock` → acquire-load `ft` → bounds check → load slot →
  `vfs_tryget` → `rcu_read_unlock`.  Callers drop the reference with
  `fdput(f)` (aliased to `vfs_close`).  Writer paths (`sys_dup`,
  `sys_dup2`, `sys_close`, `sys_pipe`, `fd_install`, `sys_fcntl`
  F_DUPFD / F_GETFD / F_SETFD, `sys_open`, `sys_shm_open`, CLOEXEC
  sweep in exec) serialise on a per-`task_files_t` writer spinlock.

  Hot path cost on x86: `lock xadd` (pin), single fence, tag load,
  `lock cmpxchg` for tryget, `lock xadd` on release.  No spinlock on
  the read path.

---

## 8. VMA list reads vs. mutation

- **Location:** `kernel/mm/mm.c` — the per-`mm_t` VMA linked list
- **Race:** `sys_mmap`, `sys_munmap`, `sys_brk` add/remove VMAs. Page
  fault handler walks the list on every fault. No synchronization.
- **Why safe on UP:** fault and syscall can't overlap.
- **Fix phase:** Phase 8.
- **Fix strategy:** per-address-space mutex for mutation. Fault walk
  uses RCU (or a seqlock if the list becomes lookup-heavy).

---

## 9. ext2 bcache slot contents — RESOLVED (Phase 7 + 7.2)

- **Phase 7.2 fix (zero-copy):** per-slot meta is now
  `{ spinlock_t wlock; uint32_t tag; uint32_t pin; }`.  Hot readers call
  `bcache_get(blk, scratch)` which does an optimistic
  atomic-fetch-add on `pin`, a full fence, and a tag load — on a hit
  that returns a `const uint8_t*` straight into the cache slot **with
  no memcpy** and the caller pins the slot until `bcache_put`.
  Writers (`bcache_fill`, called on miss-fill and write-through) use
  `trylock` + pin-then-tag-invalidate + pin-recheck, bailing silently
  on slot conflict.  On mismatch the reader falls back to `ahci_read`
  into the caller's scratch, then opportunistically publishes into
  the cache.  ext2 hot paths (`inode_get_block`, `dir_lookup`,
  `read_bgd`, `read_inode`, `ext2_vfs_read` partial/multi-block,
  `ext2_readdir`) now read the 4–128 bytes they actually need out of
  a pinned slot instead of paying a 4 KiB memcpy per block lookup.
  Bitmap walks and RMW paths still go through the `read_block`
  compat shim because they need a mutable working copy.

---

## 10. TCP PCB list walk vs. alloc/free — RESOLVED (Phase 6)

- **Fix:** `s_pcb_head` is now an RCU-protected singly-linked list.
  `tcp_recv` and `tcp_timer_tick` walk the list inside `rcu_read_lock()`
  using `rcu_dereference` on each `->next`.  `tcp_pcb_alloc` publishes
  at the head via `rcu_assign_pointer` under `s_pcb_wlock`.  `tcp_pcb_free`
  unlinks under the writer lock and defers the full teardown (txbuf,
  rxbuf, struct) to `tcp_pcb_free_rcu` via `call_rcu`, so any in-flight
  reader still dereferencing the pcb sees a live object until its
  reader section ends.

---

## 11. Pipe / PTY / TTY / unix / socket single-waiter fields — RESOLVED (Phase 3)

- **Locations (all now using lock-free MPSC wait queues):**
  - `kernel/fs/pipe.c` — `pipe_buf_t.reader` removed; uses
    read_file->waitq / write_file->waitq
  - `kernel/drivers/tty/tty.c` — `tty_t.reader` removed; uses tty->waitq
  - `kernel/drivers/tty/pty.c` — `pty_t.m_reader` removed; uses
    master_waitq and slave.waitq
  - `kernel/net/unix_sock.c` — `unix_sock_t.waiter` removed; new
    `wait_queue_t waitq` field
  - `kernel/net/socket.c` — `socket_t.waiter` removed; new
    `wait_queue_t waitq` field
- **Fix:** every blocking caller now stack-allocates a `task_we_t`,
  registers it on the relevant wait_queue_t, re-checks the condition
  under the registration (closes lost-wakeup window), then sleeps.
  Writers call `wait_queue_wake_all` which atomically drains the
  chain and fires each entry's callback.
- **SMP safety:** push is lock-free CAS, drain is lock-free xchg;
  cancellation (`wq_remove`) takes a tiny per-queue spinlock that is
  never held on the hot wake path.

---

## 12. Signal delivery free-after-exit — RESOLVED (Phase 6)

- **Fix:** `task_destroy` no longer frees the task synchronously.
  It removes from `pid_ht`, frees the pid slot, and then hands the
  task_t to `call_rcu(task_free_rcu, t)` which runs kstack/mm/files/
  kfree after a grace period.  `signal_send_group` / `signal_send_pgrp`
  walk the pgid/tgid hash buckets under the per-table spinlock, which
  prevents a concurrent `task_idx_*_remove` from splicing the bucket
  list mid-walk; any task the walker reached has already observed
  state != TASK_DEAD so its `rcu_read_lock`-equivalent bucket-lock
  window covers the signal_send call.

---

## 13. `g_current` global — RESOLVED (Phase 5)

- **Fix:** `g_current` is now a macro `(this_cpu()->current)` in
  `kernel/proc/sched.h`.  Storage lives in `cpu_t.current`; the global
  is deleted entirely.  Assignment and read sites continue to work
  unchanged (field access through a pointer is an lvalue), but every
  access now routes through `this_cpu()`, so cross-CPU reads naturally
  see their own current task.
- **vmm.c** was the only file with a stale `extern task_t* g_current;`
  forward declaration — replaced with `#include "sched.h"`.

---

## Exit criteria for Phase 9

Phase 9 (AP boot) cannot land until **every entry above** is resolved
or explicitly waived with justification.  Each phase takes a slice:

- Phase 3 resolves entries **1, 11**
- Phase 5 resolves entries **2, 4, 5, 13**
- Phase 6 resolves entries **3, 6, 10, 12**
- Phase 7 resolves entry **9**
- Phase 8 resolves entries **7, 8, 12 (task lifecycle)**

After Phase 8 finishes, re-audit this file.  Any entry still open is a
blocker for Phase 9.
