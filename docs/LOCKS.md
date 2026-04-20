# MakaOS Lock Inventory

Every spinlock/mutex in the kernel tree must be listed here with its
justification. If it isn't, either it's a bug (undocumented lock) or
the inventory is stale — either way, fix it.

Rules for adding a new entry:
1. **Hot path is covered by something else** (RCU, per-CPU, seqlock, atomics).
   The lock may only guard the cold control-plane path.
2. **No lock-free alternative is measurably better** — explain why.
3. **Critical section is tight** — no sleeps, no large allocs, no
   cross-call retention.
4. **IRQ safety** — if the lock can be taken from IRQ context, it
   must use `spin_lock_irqsave` / `spin_unlock_irqrestore`.
5. **No sleeping while held.** Enforced at runtime by the panic in
   `sched_sleep` / `sched_yield`: any attempt to block with
   `preempt_depth > 0` crashes loudly instead of deadlocking silently.

---

## `s_pid_ht_lock` — `kernel/proc/sched.c`

**Guards:** writer access to the PID hash table state pointer and its
slot array (`s_pid_ht`, published via `rcu_assign_pointer`).

**Readers:** `pid_ht_find` takes ZERO locks. RCU-protected. Readers
dereference the published state pointer and walk the hash with
`atomic_load_acq` on each slot.

**Writers:** `pid_ht_insert` (called from `sched_add` on fork/spawn/
thread) and `pid_ht_remove` (called from `sched_add_zombie` and
`sched_reap_*`). Grow is nested inside insert.

**Why a lock here:**
- Fork/exit rate is measured in hundreds per second at most.
  Uncontended fast path is a single `lock cmpxchg` (~20 cycles).
- Lock-free alternatives (CAS insert + resize coordination; per-CPU
  pending queues drained by a kthread) are significantly more code,
  prone to subtle bugs around resize, and show no measurable win
  unless fork rate exceeds ~100k/sec per CPU — which never happens.
- The hot path (reader) is already lock-free via RCU.

**IRQ-safety:** Plain `spin_lock` (not `irqsave`) because neither
insert nor remove is ever called from IRQ context. Verified:
- `sched_add` callers: init/boot/syscall context only.
- `sched_add_zombie` callers: `sys_exit`, signal fatal path, all
  process context.

**Critical section contents:** hash walk + one slot write + count
update. No sleeps, no allocations inside the lock. Grow is split into
an under-lock allocate+publish phase and an out-of-lock reclaim phase
that calls `synchronize_rcu`. See `pid_ht_grow_locked` / `pid_ht_reclaim_state`.

**SMP-readiness:** correct as-is. Contention only appears if thousands
of CPUs fork simultaneously, which is not a real workload.

---

## `g_pmm_lock` — `kernel/mm/pmm.c` (Phase 4 — depot/slow-path lock)

**Phase 4 status: landed.**  `g_pmm_lock` is now the **depot lock**
behind the per-CPU allocator fast paths.  Hot-path `kmalloc/kfree`
and order-0 `pmm_buddy_alloc/free` no longer touch it — they hit
the lockless `cpu_slot[cls]` (slab) or `pcp_hdr` (pageset) via
`cmpxchg16b` on `%gs:offset`-relative per-CPU memory.  Only the
slow paths (refill / overflow / demote / shrinker / order ≥ 1
buddy / refcount / pin) still acquire the lock.

Acceptance measurement (Phase 4H self-test, `slab_pcpu_selftest`,
4-CPU × 50k mixed-size alloc/free pairs): slab fast-path hit rate
9999/10000 (99.99 %), pcp hit rate 100 %.

**Guards:** all buddy free lists, slab cache heads
(partial / full / empty), per-frame refcount array, per-frame
pincount array, slab tracker array, slab page-state (`on_list`).
Under SMP the fast paths **never** see contention on this lock;
only the batched refill (~1 in 16–32 allocs after warmup), the
state-transition helpers (`pmm_slab_demote_or_keep`, `park_full`,
`grab_partial`, etc.), the refcount/pin APIs, and order ≥ 1 buddy.

**Readers (lock-free):**
- `pmm_ref_get`, `pmm_pin_get`, `pmm_is_slab_ptr` — single aligned
  loads.  Callers needing a stable read must hold a higher-level lock.

**Writers (slow-path only):**
- `pmm_buddy_alloc/free` for order ≥ 1
- `pmm_slab_grab_partial/empty/grow`, `pmm_slab_park_partial/full/empty`,
  `pmm_slab_demote_or_keep`
- `pmm_ref_inc/dec`, `pmm_pin/unpin`
- `pmm_slab_shrink_all_locked` (shrinker and pressure hook)
- `pmm_buddy_alloc_locked_for_pcp` / `pmm_buddy_free_locked_for_pcp`
  (called under `pmm_pcp_lock / _unlock` from `pcp.c` to batch a
  whole refill or drain in one lock acquire)

**Hot path (Phase 4 takes no lock here):**
- `kmalloc` → `slab_pcpu_alloc` → cmpxchg16b on `%gs:cpu_slot[cls]`
- `kfree` → `slab_pcpu_free` → cmpxchg16b on `%gs:cpu_slot[cls]` OR
  (cross-CPU) `pmm_slab_free` (which does take `g_pmm_lock`)
- `pmm_buddy_alloc(0)` → `pcp_alloc` → cmpxchg16b on `%gs:pcp_hdr`
- `pmm_buddy_free(addr, 0)` → `pcp_free` → cmpxchg16b on `%gs:pcp_hdr`

**IRQ-safety:** MUST use `spin_lock_irqsave` / `spin_unlock_irqrestore`.
`pmm_ref_dec` is reachable from the page fault handler which may run
with some IRQs masked, and `kfree` is called from IRQ-context code
paths in the future (deferred work callbacks, Phase 6).

**Critical section contents:** buddy free-list operations (O(MAX_ORDER)
splits/merges), slab list manipulation, memset-of-slab-header, tracker
array updates. No sleeps, no cross-call retention. The one costly op
is buddy split/merge which touches multiple free lists — still
bounded O(MAX_ORDER=32) and does no I/O.

**SMP-readiness:** Phase 4 landed — this is the depot lock behind
per-CPU allocator magazines (slab) + pcp (buddy order-0).  Fast
paths never touch it; refill/drain hit it ~1 in 16–32 allocs after
warmup.  Measured cross-CPU hit rate: 99.99 % (Phase 4H self-test).

**History:** see `docs/PHASE4_REDESIGN.md` for the full design.

---

## `cpu_t.rq_lock` — `kernel/proc/sched.c` (per-CPU, MAX_CPUS instances)

**Guards:** one CPU's run queue (MLFQ `heads[]`/`tails[]`), sleep
list, zombie list, and `reschedule_pending` flag. Each CPU has its
own instance; no global scheduler lock exists.

**Readers (lock-free):**
- `pid_ht_find` — not a reader of rq state, uses RCU on a separate
  table.
- `sched_for_each` — actually a reader but takes each CPU's rq_lock
  in turn because it needs to walk the lists safely. Cold path
  (only /proc enumeration and broadcast kill(-1)).

**Writers:** `sched_add`, `sched_add_zombie`, `sched_reap_*`,
`sched_wake`, `sched_sleep`, `sched_yield`, `sched_tick`, `do_switch`,
`sched_wait_pid`, `sched_poll_pid`, `sched_has_child_zombie`.

**Why a lock per CPU, not a global:**
- The alternative (one global sched lock) would serialize every
  context switch across all CPUs — unacceptable for SMP scalability.
- Per-CPU locks allow CPUs to run their own schedulers in parallel
  with zero contention on the common path (sched_tick, sched_yield,
  sched_sleep, local wakes all touch only the running CPU's lock).
- Cross-CPU wakes (`sched_wake(t)` where `t->home_cpu != current_cpu`)
  do take the target's lock, but that's a rare path compared to
  local scheduling.

**IRQ-safety:** MUST use `spin_lock_irqsave` / `spin_unlock_irqrestore`.
`sched_tick` runs in the timer ISR (already IRQ-disabled but saving
flags is uniform), and `sched_wake` can be called from hardware IRQ
completion paths (AHCI, keyboard, network). A non-irqsave variant
would deadlock the moment a timer IRQ fired while a sched_wake was
mid-op.

**Critical section contents:** MLFQ list walk / push / pop, sleep
list insert / remove, zombie list insert / remove, reschedule flag
set. No allocations inside the lock. No sleeps (enforced by the
`sched_sleep_while_preempt_disabled_panic` in sched_sleep).

**Release-before-switch invariant:** `do_switch` releases the rq_lock
BEFORE calling `context_switch`. Holding the lock across the context
switch would deadlock — the new task might try to take the same lock
before the old task's unlock ever runs.

**SMP-readiness:** correct. Every scheduler operation is properly
locked. The cross-CPU wake path is a simple lock-on-target pattern.

---

## `g_fb_lock` — `kernel/drivers/video/fb.c`

**Guards:** `fb_term_putc` / `fb_term_write` serialization across CPUs.
Protects `g_fb_col`, `g_fb_row`, the framebuffer scroll memcpy, and
the per-cell pixel writes that back `fb_putc_at`.

**Added:** Phase 9-5 (SMP bring-up).

**Why a lock here:**
- Pre-SMP, `fb_term_putc` wrapped its body in `preempt_disable` /
  `preempt_enable`. On UP that's sufficient because only one CPU can
  be executing any kernel code at a time. Under SMP it's useless:
  preempt_disable only blocks the LOCAL CPU from switching tasks; it
  has zero effect on a remote CPU calling `fb_term_putc` concurrently.
- Observed symptom before the fix (`-smp 4`, round-robin placement):
  user-visible bash prompts would render on some boots and not on
  others, depending on whether another CPU happened to be mid-scroll
  or mid-character-write when bash's prompt hit the code path. Cursor
  coordinates tore, scroll memcpy ran concurrently with character
  writes, and whole character cells vanished.
- Lock-free alternatives considered and rejected:
  - **Per-CPU fb buffers with a merge kthread**: adds latency, breaks
    the "any printk comes out instantly" panic-path invariant.
  - **Seqlock on g_fb_col/row**: doesn't cover the pixel memory that
    `fb_putc_at` and `fb_term_scroll` write. Readers don't exist —
    this is all writer contention — so seqlock is the wrong primitive.
  - **Ring of "draw commands" consumed by one CPU**: serialises worse
    than just taking the lock, and complicates the panic path.

**IRQ-safety:** `spin_lock_irqsave` / `spin_unlock_irqrestore`. The
panic-print path inside `#PF` / `#GP` / `#UD` exception handlers calls
`fb_term_putc` from IRQ/trap context — a plain `spin_lock` could
deadlock if the holder gets interrupted by a fault that also tries to
print.

**Critical section contents:** one character's worth of
`fb_term_putc_locked`: a few field reads/writes on `g_fb_col/row`,
one call to `fb_putc_at` (which writes 8×16 pixels), and on wrap or
`\n` maybe a `fb_term_scroll` (a `rep movsb`-sized memcpy + a
final-row clear). No sleeps, no allocations, no cross-CPU reads.

**Call-site granularity:** the batched writer `fb_term_write` takes
the lock **once** for the whole buffer, not per-byte. bash writing a
4 KiB line takes the lock once (4 KiB `fb_term_putc_locked` loop),
not 4 096 times. This is the hot path.

---

## Planned locks (not yet added — for Phase 3–8)

Each entry will be added as the phase lands, with the same justification
template. This list is for tracking the *plan*, not the code.

### Phase 3 — Lock-free data structures

- **Wait queues (MPSC):** will replace the current `wait_queue_t`
  linked-list code. Lock-free via atomic `xchg` on the tail. No
  spinlock needed.
- **Seqlocks:** readers never block. Writers increment a version
  counter. No spinlock needed in the primitive itself, though a
  seqlock may be combined with a spinlock for multi-writer scenarios.

### Phase 4 — Per-CPU slab + pcp

- **Slab magazine depot:** one spinlock per size class, taken only
  when a CPU's magazine underflows or overflows (batched refills). Per-CPU
  magazines themselves are lock-free (owner-only access under
  preempt_disable).
- **PMM buddy freelists for order ≥ 1:** one spinlock for the buddy
  freelists. Order-0 allocations go through a per-CPU pageset first
  and only hit the global buddy on refill, so this lock is cold.

### Phase 5 — Per-CPU scheduler

- **Per-CPU run queue:** owner-only access under preempt_disable. No
  lock for normal operation.
- **Cross-CPU wake mailbox:** MPSC queue (lock-free).
- **Work-stealing:** atomic CAS on the victim's head pointer. No
  spinlock.

### Phase 6 — RCU-ize global tables

- **pgid/tgid/sid task index:** RCU reads, single writer lock per
  table for inserts/removes (same pattern as `s_pid_ht_lock`).
- **Signal handlers per task:** RCU-protected. Sigaction is called
  essentially once per process, so copy-on-write at install time is
  nothing.
- **Mount table, route table, DNS cache:** RCU reads, writer lock on
  mutation.

### Phase 7 — Seqlock the hot caches

- **ext2 bcache slot:** one seqlock per slot. Readers never block.
- **System clock / TSC conversion:** seqlock.

### Phase 8 — fd table + VFS + ext2 metadata

- **fd table mutation:** per-task_files_t mutex. Read path is
  RCU-protected.
- **ext2 inode lock:** per-directory mutex. Reads are RCU via the
  inode cache.
- **VMA list mutation:** per-address-space mutex. Page fault handler
  walks under RCU.

### Phase 9 — SMP bring-up

- **IPI / TLB shootdown coordination:** per-mm cpumask with atomic
  ops. No spinlock in the hot path.

---

## Phase 6 — RCU-ized global tables (landed)

All of the following are **writer-only** spinlocks.  The read path is
lock-free (`rcu_read_lock` = preempt_disable + plain pointer load +
hash walk).  Writers acquire the lock to serialise against other
writers, build a fresh table via copy-on-write, publish the new
pointer via `rcu_assign_pointer`, and hand the old table to `call_rcu`
for deferred free.  Mutations are rare (O(sockets)) so the O(cap) COW
cost is amortised; the reader hot path is the only thing that matters.

### `s_arp_wlock` — `kernel/net/arp.c`
Guards the `s_arp` RCU-published table pointer.  Writer path runs
only inside `arp_recv` (NIC softirq).  `arp_lookup`, called on every
outgoing packet, does **zero atomics** — just `rcu_read_lock`, hash
walk, `memcpy(mac, 6)`, `rcu_read_unlock`.

### `s_udp_wlock` — `kernel/net/socket.c`
Guards the `s_udp` port table pointer.  Writers are
`udp_table_register` / `udp_table_remove`.  `udp_table_find` runs
inside `rcu_read_lock()` from `socket_deliver_udp` on every UDP
packet.  `socket_t` teardown is `call_rcu`-deferred via
`sock_free_rcu` so the reader's RX enqueue cannot race a close.

### `s_unix_ns_wlock` — `kernel/net/unix_sock.c`
Guards the `s_unix_ns` namespace table pointer.  `unix_sock_close`
defers the full teardown (peer unlink, backlog drain, dgram drain,
ancillary fd close, struct free) via `call_rcu(unix_sock_free_rcu)`,
so a concurrent `connect`/`sendto` inside `rcu_read_lock()` sees a
consistent peer until it exits the reader section.

### `s_namespace_lock` — `kernel/mm/shmem.c`
Guards the `s_namespace` table pointer.  `shmem_ns_find` walks
lock-free inside `rcu_read_lock()` and bumps the found object's
refcount via `shmem_tryget` — a CAS loop that increments only if the
count is currently > 0, so it cannot resurrect a freed object.
`shmem_t.refcount` is now atomic.  Final free runs from
`shmem_free_rcu` after a grace period.

### `s_pcb_wlock` — `kernel/net/tcp.c`
Guards the `s_pcb_head` linked-list head and every `->next` link.
`tcp_recv` and `tcp_timer_tick` walk the list inside `rcu_read_lock()`
with `rcu_dereference` on each `->next`, which keeps the pcbs alive
via the `call_rcu`-deferred `tcp_pcb_free_rcu` path.  `tcp_pcb_alloc`
publishes at the head via `rcu_assign_pointer`.

### `task_idx_t.lock` (×3) — `kernel/proc/sched.c`
One IRQ-safe spinlock per `task_idx_t` (pgid, tgid, sid).  Held on
every `task_idx_*_insert` / `task_idx_*_remove` / `task_idx_*_walk`.
Signal delivery (`signal_send_group` / `signal_send_pgrp`) walks the
bucket list inside the table lock so the callback sees a consistent
list.  Not RCU because signals are cold-path and the bucket walk +
per-task callback is already microsecond scale.  The per-bucket
doubly-linked list pointers in `task_t` (`pg_prev/pg_next`,
`tg_prev/tg_next`, `sid_prev/sid_next`) are also covered by these
locks.

### `epoll_state_t.lock` (per-fd) — `kernel/syscall/syscall.c`
Guards the `slots[]` / `cap` / `count` fields of an individual epoll
fd.  **Not IRQ-off**: the wake callback `epoll_wake_func` only touches
`has_ready` + `wq` and never takes this lock, so the watched fd's
waitq can fire from IRQ context without serialising on epoll.
`epoll_wait` collects matching events into a temporary kernel buffer
under the lock, then releases the lock before `copy_to_user` so a
user-page fault never happens with a spinlock held.

### RCU-deferred free paths (not locks, but part of Phase 6)

- `task_destroy` → `call_rcu(task_free_rcu)` frees kstack, mm,
  files, cwd, unveil, and the `task_t` itself after a grace period.
- `tcp_pcb_free` → `call_rcu(tcp_pcb_free_rcu)` (txbuf, rxbuf, pcb).
- `sock_close` → `call_rcu(sock_free_rcu)` (UDP RX queue + socket).
- `unix_sock_close` → `call_rcu(unix_sock_free_rcu)` (full teardown).
- `shmem_unref` on last ref → `call_rcu(shmem_free_rcu)` (pages +
  struct).

---

## Running count

| Phase | Spinlocks added | Total |
|---|---|---|
| 1 | 0 | 0 |
| 2 | 1 (pid_ht writer) | 1 |
| 3 | 0 (all lock-free) | 1 |
| 4 | 1 global g_pmm_lock (temporary until redesign lands — see PHASE4_REDESIGN.md) | 2 |
| 5 | 1 per-CPU rq_lock (MAX_CPUS=64 instances but 1 design entry) | 3 |
| 6 | 8 writer-only (arp / udp / unix-ns / shmem-ns / tcp-pcb / 3×task_idx) + 1 epoll-per-fd | ~11 |
| 7 | 0 (seqlocks) | ~11 |
| 8 | per-object mutexes (not global) | ~11 + per-object |
| 9 | 1 (g_fb_lock — Phase 9-5 SMP bring-up) | ~12 |
| 9-6 | 0 (audit only — children list Treiber stack, AHCI MPSC) | ~12 |

After Phase 4 proper lands (post-SMP, per-CPU magazines), g_pmm_lock
collapses to a rarely-taken depot lock and doesn't change the total
count — just moves off the hot path.

The per-object mutexes in Phase 8 aren't in the "global spinlock" count
— they're finer-grained and there's one per fd table / per inode / per
address space. Contention on them is bounded by how many threads
share that specific object, which is typically 1–2.

**Target final global spinlock count: under 20.**
Anything beyond that, I will have to justify. If it creeps higher,
that's a design signal to step back and reconsider.
