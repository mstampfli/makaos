# MakaOS SMP Architecture — Lock-Minimized Design

## Guiding principles (in priority order)

1. **No lock in the common path.** Fast paths are either per-CPU (no sharing) or lock-free (atomic ops).
2. **Readers never block writers, writers never block readers** — RCU or seqlock for read-mostly data.
3. **Shard aggressively.** When you must share, partition by CPU or by hash so contention is local.
4. **Locks are a last resort** for control-plane operations (create/destroy), never for data plane (read/write).

The design is walked through subsystem by subsystem. Each picks the technique and justifies it.

---

## Layer 0 — SMP primitives (`smp.h`, real implementations)

The stubs added earlier become real:

```c
typedef struct { volatile uint32_t locked; } spinlock_t;

static inline void spin_lock(spinlock_t* l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (l->locked) __asm__ volatile("pause");
}
static inline void spin_unlock(spinlock_t* l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
```

Ticket lock variant for high-contention paths (FIFO fairness, no starvation). But the goal is to never use them in hot code.

**Per-CPU accessor**: GS base points at each CPU's `cpu_t` struct.
```c
struct cpu_t {
    uint32_t      id;
    task_t*       current;
    task_t*       idle;
    run_queue_t   rq;
    slab_caches_t slab;
    pmm_pcplist_t pcp;   // per-CPU pageset
    uint32_t      preempt_depth;
    uint8_t       irq_pending[IRQ_COUNT];
    // ...
};
#define this_cpu() ((cpu_t*)__builtin_ia32_rdmsr(MSR_GS_BASE))
```

**Memory barriers**: real `mfence`/`lfence`/`sfence` under SMP. Cheap on x86 because of its strong memory model.

**Atomics**: already working (`__atomic_*`).

---

## Layer 1 — Lock-free infrastructure

These are the building blocks. Once they exist, most subsystems use them instead of locks.

### 1. RCU (Quiescent-State-Based RCU)

Classic Linux trick. Readers take zero synchronization:
```c
rcu_read_lock();       // no-op: just disables preemption
p = rcu_dereference(ptr);
// use p
rcu_read_unlock();     // re-enable preemption
```

Writers do copy-on-write and wait for a grace period (all CPUs to have passed through a "quiescent state" like the idle task or a context switch):
```c
new = kmalloc(...);
*new = *old;
modify(new);
rcu_assign_pointer(ptr, new);
synchronize_rcu();     // wait until every CPU has context-switched once
kfree(old);
```

**Why this is gold**: readers pay nothing. Zero atomic ops, zero cache misses on the lock line, just a plain pointer dereference. Writers are rare in read-mostly data (PID table, mount table, signal handlers) so the copy cost is amortized.

**Where it's used**:
- PID hash table (lookups vastly outnumber inserts)
- pgid/tgid/sid index tables
- mount table / VFS mount points
- signal handlers (sigaction is effectively never called)
- task_files_t when shared across threads (read-mostly)
- epoll watch lists

### 2. Sequence locks (seqlock)

Writers increment a version counter before and after a write. Readers read the version, then the data, then the version again — if versions match and are even, the read was consistent; otherwise retry.

```c
do {
    seq = seq_begin(&sl);
    // read data
} while (seq_retry(&sl, seq));
```

**Why**: readers never spin on writers. Perfect for high-frequency reads with occasional writes where RCU's copy cost is too high (data is large).

**Where it's used**:
- ext2 block cache slots (read path must be hot)
- System time / monotonic clock
- IP routing cache

### 3. Lock-free MPSC queue (multi-producer, single-consumer)

Any CPU can push, only one CPU (the owner) pops. Push uses a single atomic `xchg` on the tail. Pop is plain loads.

**Where**:
- Wake queues for sleeping tasks
- Incoming network packets to the netstack thread
- Deferred free list for RCU callbacks

### 4. Lock-free SPSC queue (single-producer, single-consumer)

Even faster than MPSC. One writer, one reader. Pure atomic loads/stores with release/acquire ordering, no CAS needed.

**Where**:
- Per-CPU work queues for work stealing
- AHCI request submission ring (one submitter, one completer thread per device)

### 5. Atomic bitmap

Already have the primitives. Used wherever "is this set-membership" is the question: signal pending, fd flags, wait queues, CPU masks.

### 6. Hazard pointers (alternative to RCU for some cases)

Each reader publishes the pointer they're currently reading. Writers check all published hazards before freeing. More complex than RCU but bounded memory (RCU can batch-defer free until idle CPUs show up).

**Where**: the fd table lookup path. fd table has to be reclaimed promptly on process exit.

---

## Layer 2 — Per-CPU subsystems (the biggest wins)

This is where most of the performance comes from: **move shared data into per-CPU slots so no synchronization is ever needed for common operations.**

### Scheduler → per-CPU run queues with work stealing

Every CPU has its own MLFQ:
```c
struct cpu_t {
    run_queue_t rq;   // private — only this CPU touches it
    task_t*     current;
    task_t*     idle;
};
```

- **`sched_yield`, `sched_tick`, `sched_sleep`**: touch only `this_cpu()->rq`. **Zero locks.**
- **`sched_wake`**: tries to wake on the task's "home CPU". If that CPU is busy, push to its per-CPU mailbox via MPSC. If the target CPU is idle, send an IPI. No global queue.
- **Load balancing / work stealing**: when a CPU's queue is empty, it looks at other CPUs' queues (via atomic load of their head pointer) and steals one task. Steal requires an atomic CAS on the victim's head — the ONLY place the scheduler uses a lock, and only when this CPU has literally nothing to do.

Result: scheduler overhead per tick = zero atomic ops on the common path.

### Slab allocator → per-CPU magazines

Each CPU has a small freelist per size class (8, 16, 32, ..., 4096). Alloc/free hit the local freelist first — zero sync. When empty/full, exchange with a global "depot" via a lock taken only per-batch.

Linux calls this "magazine allocator" (originally from Solaris). The amortized cost of `kmalloc(64)` under contention is one local pointer bump — **faster than a byte loop**.

```c
static inline void* kmalloc_fast(size_t sz) {
    cpu_t* c = this_cpu();
    int idx = size_class(sz);
    if (c->slab.free[idx]) {
        void* p = c->slab.free[idx];
        c->slab.free[idx] = *(void**)p;
        return p;
    }
    return kmalloc_slow(sz);  // refill from global depot
}
```

Preempt must be off during this path (we're touching per-CPU state). A local `preempt_disable`/`preempt_enable` brackets the fast path — cheaper than any lock because it's just a non-atomic counter on a local variable.

### Physical memory allocator → per-CPU page cache (pcp)

Linux's "per-CPU pageset". Each CPU caches 64 free order-0 pages locally. `pmm_buddy_alloc(0)` pops one; `pmm_buddy_free` pushes one. No global lock until the batch overflows/underflows.

Higher-order allocations (2+ pages) still go to the global buddy. They're rare enough that the lock is fine.

### IRQ waiters → per-CPU pending counts + per-IRQ MPSC wake queue

The waiter list already uses stack-allocated linked-list nodes. Now: the list head is per-IRQ (not per-CPU), but wakes send tasks to the MPSC of their home CPU. The IRQ handler itself runs on one specific CPU (steered by the IOAPIC), so the waiter list for that IRQ is naturally owned by that CPU — no cross-CPU access in the common path.

### kheap, pmm, IRQ — summary

All lock-free on the hot path. Locks only on slow paths (depot refill, large alloc, IRQ steering change).

---

## Layer 3 — Read-mostly global data → RCU

These are the tables every CPU reads constantly but writes rarely:

### PID hash table → RCU protected

Lookups (in `kill`, `wait`, `signal_send`) become:
```c
rcu_read_lock();
task_t* t = pid_ht_find(pid);   // walks the hash table — no lock
if (t) rcu_read_unlock_and_use(t);
else   rcu_read_unlock();
```

Inserts/removes acquire a single global spinlock (rare: only on fork/exit). Removal uses `synchronize_rcu()` before the actual kfree of the task, so any in-flight readers finish first.

### pgid/tgid/sid task index → RCU

Same treatment. `signal_send_pgrp` walks an RCU-protected list, zero synchronization on every signal.

### Mount table, route table, DNS — RCU

### sigaction handlers — RCU per task

Handlers change rarely (bash installs SIGCHLD once). Read on every signal delivery. RCU-read = plain load.

---

## Layer 4 — Sequence-locked hot structures

### ext2 block cache slots → seqlock per slot

```c
typedef struct {
    seqlock_t seq;
    uint32_t  tag;
    uint8_t   data[4096];
} bcache_slot_t;

// Reader:
uint32_t seq;
do {
    seq = seq_begin(&slot->seq);
    if (slot->tag != blk) break;
    __builtin_memcpy(user_buf, slot->data, 4096);
} while (seq_retry(&slot->seq, seq));

// Writer (from disk read completion):
seq_write_begin(&slot->seq);
slot->tag = blk;
__builtin_memcpy(slot->data, dma_buf, 4096);
seq_write_end(&slot->seq);
```

Readers never block. Writers are rare (cache miss, not cache hit). Retries only happen on the rare collision where another CPU wrote the same slot during our read — then we redo, which is 4µs.

### System clock → seqlock

`tsc_read_ns()` + wallclock conversion. Very frequent reads, rare writes when NTP adjusts.

---

## Layer 5 — Subsystems that need traditional locks (minimal)

Not everything is lock-free. Some control-plane operations need a real lock, but they're kept tiny and off the hot path:

- **fd table mutation** (`dup`, `close`, `open`) — per-task_files_t mutex. Read path (`fd_to_file`) uses RCU lookup. Mutations are rare.
- **File creation / unlink / rename** (ext2 metadata writes) — filesystem inode mutex per directory. Reads are RCU (inode cache).
- **VMA list mutation** (`mmap`, `munmap`) — per-address-space mutex. Page fault handler walks the VMA list under RCU.
- **Task creation / destruction** — global `tasklist_lock` held briefly. Everyone else sees the task via RCU pointer.

Every mutex is annotated with what it protects and why it can't be lock-free. If it can't be justified, rethink.

---

## Layer 6 — Cross-CPU coordination

Some operations are fundamentally cross-CPU and can't be avoided:

### TLB shootdowns

When unmapping a page that may be cached in another CPU's TLB, an IPI (inter-processor interrupt) is needed to force them to flush. Minimize these:
- **Lazy TLB** for kernel threads: they don't have their own address space, no TLB flush needed on context switch.
- **Batch shootdowns**: accumulate multiple unmaps into one IPI.
- **Targeted IPI**: only shoot down CPUs that actually have the address space loaded (tracked via a per-mm cpumask).

### IPI for wakeups

When a task wakes and its home CPU is idle, an IPI is needed to wake it up. One atomic bit set + one `wrmsr` to LAPIC ICR. Unavoidable, but rare — only when the target was actually in `hlt`.

### AP boot

One-time setup. Not a performance concern.

---

## Specific subsystem plan (what gets rewritten)

| Subsystem | Technique | Status |
|---|---|---|
| Scheduler run queues | per-CPU, work-stealing | Full rewrite |
| Task current/preempt | per-CPU | Rewrite |
| PID hash | RCU | Wrap reads in rcu_read_lock, add synchronize_rcu on delete |
| pgid/tgid/sid index | RCU | Same |
| Signal pending | atomic bitmap (already done) | Keep |
| Signal handlers | RCU | Wrap reads |
| Slab allocator | per-CPU magazines | Full rewrite |
| pmm order-0 | per-CPU pcp | Add pcp layer on top of buddy |
| pmm higher order | global lock | Keep (rare) |
| fd table | RCU read, mutex write | Wrap reads |
| VFS file ops | already per-file | Keep |
| VMA list | RCU read, mutex write | Wrap reads |
| Page fault handler | RCU for VMA lookup | Restructure |
| ext2 bcache | seqlock per slot | Add seq counter |
| ext2 metadata | per-inode mutex | Add locks |
| TCP PCB table | RCU | Wrap reads |
| Unix socket namespace | RCU | Wrap reads |
| ARP cache | RCU | Wrap reads |
| IRQ waiters | per-CPU MPSC | Restructure |
| AHCI request queue | SPSC | Already close |
| Timer wheel | per-CPU | Rewrite |
| Wait queues | MPSC | Rewrite wait.h |

---

## Phasing plan

Strict order so every phase is testable independently. Each phase boots and passes the existing workload before moving to the next.

**Phase 1 — real SMP primitives**
- Replace smp.h stubs with real atomics, spinlocks, memory barriers
- Add `cpu_t` struct and GS-based `this_cpu()`
- Add `preempt_disable`/`enable` working with `this_cpu()->preempt_depth`
- No behavioral change: single CPU still works identically

**Phase 2 — RCU framework**
- `rcu_read_lock`, `rcu_dereference`, `rcu_assign_pointer`
- `synchronize_rcu` using quiescent-state tracking
- `call_rcu` for deferred free
- Wrap a few read-heavy lookups (start with PID table) to validate the framework

**Phase 3 — lock-free data structures**
- MPSC queue
- SPSC queue
- Seqlock
- Replace wait queues with MPSC

**Phase 4 — per-CPU slab + pcp**
- Per-CPU magazine layer on top of current kheap
- Per-CPU pageset for order-0 pmm
- This is where the biggest allocation speedup comes from

**Phase 5 — per-CPU scheduler**
- Split MLFQ into per-CPU runqueues
- Work stealing in idle loop
- Migrate `g_current` to `this_cpu()->current`
- Wake path posts to target CPU's mailbox via MPSC

**Phase 6 — RCU-ize the global tables**
- PID hash, pgid/tgid/sid index
- Signal handlers per task
- Mount / route / DNS caches
- Each one becomes read-lock-free

**Phase 7 — seqlock-ize hot caches**
- ext2 bcache
- Clock

**Phase 8 — fd tables + VFS + ext2 metadata with proper per-object locks**
- Locks only on mutation paths
- RCU for reads

**Phase 9 — AP boot** (in progress — 9-1 through 9-4c done)
- 9-1 ✅ per-CPU `cpu_t` + `%gs:0` self-pointer + one-insn `this_cpu()`
- 9-2 ✅ ACPI MADT parse → `g_acpi.cpus[]` + RSDP via UEFI config table
- 9-3 ✅ x2APIC rewrite of `lapic.c`, IPI vectors 0x40/0x41/0x42 reserved,
          `g_num_cpus` (online) vs `g_acpi.cpu_count` (present) split
- 9-4a ✅ per-CPU TSS embedded in `cpu_t`, shared GDT sized
          `6 + 2*MAX_CPUS`, syscall entry loads RSP0 via `[gs:off]`
          (offset generated by `asm_offsets.c` Linux-style)
- 9-4b ✅ real-mode → long-mode AP trampoline (one 4 KiB flat blob,
          packaged into ELF via `objcopy -I binary`)
- 9-4c ✅ `smp_boot_aps()` launcher + `cpu_init_ap()` entry point.
          Under `-smp 4`, all four CPUs reach `cpu_init_ap`; APs spin
          in a `pause` loop bumping `rcu_qs_count` (halting with IRQs
          off would deadlock `synchronize_rcu()` — see PHASE9_HANDOFF).
- 9-5 ⏳ IPI handlers (reschedule + call-function) + per-CPU LAPIC
          timer + AP idle task.  First phase where APs actually run
          user-visible work.
- 9-6 ⏳ TLB shootdown (per-mm cpumask, batched range IPI, lazy TLB
          for kernel threads).
- 9-7 ⏳ multi-CPU soak: `stress -c 4`, per-process DOOM, cross-CPU
          signals, cleanup of any "Phase 9 TODO" comments.

---

## Performance expectation (back-of-envelope)

- **Slab alloc hot path**: ~5 ns (one pointer bump, preempt off) vs current ~50 ns (slab lookup). **10× faster.**
- **Scheduler tick**: ~20 ns per tick per CPU (per-CPU queue walk) vs current ~100 ns (MLFQ + global checks).
- **PID lookup**: ~5 ns (RCU-protected hash lookup) vs current ~20 ns (same hash but with potential cache-line contention under SMP).
- **Signal send to pgrp**: ~50 ns × members (atomic bit set only) vs a hypothetical lock version at 200 ns × members.
- **fb_term_scroll**: already `rep movsb`, unchanged.
- **ext2 bcache hit**: ~100 ns (seqlock read + memcpy) vs a mutex version at 500+ ns.

No hot path takes a lock. Anywhere contention matters, the architecture either splits the data per-CPU or uses RCU/seqlock for reads.

---

## Target configuration

- **MAX_CPUS = 64.** Enough to run realistic SMP workloads without wasting BSS on per-CPU arrays sized for hypothetical 1024-CPU machines. Raising the ceiling later is a single `#define` change plus BSS growth — no architectural impact.
