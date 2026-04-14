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

## Running count

| Phase | Spinlocks added | Total |
|---|---|---|
| 1 | 0 | 0 |
| 2 | 1 (pid_ht writer) | 1 |
| 3 | 0 (all lock-free) | 1 |
| 4 | 1 slab depot + 1 buddy (cold) | 3 |
| 5 | 0 (all per-CPU) | 3 |
| 6 | 3 (pgid/tgid/sid writers) + handful (signal/mount/route) | ~8 |
| 7 | 0 (seqlocks) | ~8 |
| 8 | per-object mutexes (not global) | ~8 + per-object |
| 9 | 0 | ~8 |

The per-object mutexes in Phase 8 aren't in the "global spinlock" count
— they're finer-grained and there's one per fd table / per inode / per
address space. Contention on them is bounded by how many threads
share that specific object, which is typically 1–2.

**Target final global spinlock count: under 20.**
Anything beyond that, I will have to justify. If it creeps higher,
that's a design signal to step back and reconsider.
