# Phase 4 — Per-CPU allocators (deferred)

## Status

**Deferred.** The initial implementation was tried and reverted after it
made the common path slower, not faster (see "What went wrong below").
A redesign is documented here; the actual work is postponed until after
SMP bring-up (Phase 9) so we can measure on a real multi-CPU workload.

In the meantime, the existing allocators (`pmm_buddy_alloc`,
`pmm_slab_alloc`, `kmalloc`) are wrapped in a global `g_pmm_lock`
(IRQ-safe) for SMP safety — slow under contention, but correct.

## What went wrong in the first attempt

The reverted commit introduced per-CPU magazine slab + per-CPU pageset
on top of the existing slab allocator. In theory: hot path = stack pop
from a local freelist, ~10 cycles.

Three concrete problems killed the win:

1. **Refill thrashing.** The refill path pulled objects from the global
   depot one at a time in a loop, re-acquiring `preempt_disable` around
   each individual `depot_alloc` call. On a cold magazine (every boot,
   every new allocation class), one logical `kmalloc` triggered
   `SLAB_MAG_BATCH=16` depot allocations, each wrapped in its own
   preempt scaffolding. Instead of amortising a slow call over many
   fast ones, it paid 16× the cost per miss.

2. **Double enable/disable per alloc.** The fast path had two
   `preempt_disable`/`enable` pairs: one for the first try-pop, another
   for the post-refill try-pop. Each pair does `incl + cmp + je + decl
   + setne + (maybe) call`. That's ~15 cycles × 2 on top of the
   existing slab call — worse than the original single indirect call
   to `pmm_slab_alloc`.

3. **Free-path class lookup.** To pick the right magazine bin on
   `kfree`, the reverted code called `pmm_slab_size_of(ptr)` which
   does a physical-to-frame conversion plus an array lookup. The
   original `kfree` went straight to `pmm_slab_free` with zero class
   derivation. Net: kfree was SLOWER with the magazine than without.

The observable symptom was "display server loads slower, bash echo
feels laggier". Confirmed by reverting the patch and the feel
returned to pre-Phase-4 speed.

## Rules for the retry

The redesign below must deliver on all of these or the retry is abandoned:

1. **Hot path = fewer instructions than `pmm_slab_alloc`.** Measure in
   cycles, not lines of C.
2. **One preempt_disable/enable pair per alloc.** Not two, not three.
3. **No class lookup on kfree.** The class must be inferable in O(1)
   from the pointer itself (ideally: the magazine slot stores a tag).
4. **Refill is a single burst.** Leave preempt disabled for the whole
   refill if the depot call is short, or release just once across the
   batch if the depot can block.
5. **Per-CPU cold state starts pre-warmed.** First access to a CPU's
   magazine should not trigger a full refill on cold boot; the BSP
   can seed the common size classes (8, 16, 32, 64, 128) during
   `cpu_init_bsp`.
6. **Measure before committing.** Add counters: fast_path_hits,
   slow_path_misses, refill_bursts, drain_bursts. Print on demand.
   If fast-path hit rate is below 95% on any realistic workload, the
   design is wrong.

## The redesign

### Per-CPU slab magazine (redesigned)

```c
// In cpu.h — per-CPU state
#define SLAB_MAG_CLASSES 10    // must match KMALLOC_CACHE_COUNT
#define SLAB_MAG_DEPTH   64    // max objects per class per CPU

typedef struct {
    void*    stack[SLAB_MAG_CLASSES][SLAB_MAG_DEPTH];
    uint32_t count[SLAB_MAG_CLASSES];
} slab_mag_t;
```

**Key structural change:** fixed-size array per class, not a linked
list of free objects. Reasons:

- Linked-list pops touch the free object's memory (to read its `next`
  pointer). That's a cache line miss on every pop of a cold object.
  An array pop reads only the magazine's own memory — one cache line
  for the whole class's stack, which stays hot.
- Push/pop become `stack[cls][--count]` / `stack[cls][count++]` —
  two instructions, no pointer chasing.
- The class index IS the array dimension. `kfree(ptr)` derives the
  class from `pmm_slab_size_of(ptr)` ONCE on the free path, then
  pushes. If that proves too slow, we can store the class tag next
  to the pointer on the magazine stack (`struct { void* p; uint8_t
  cls; }`) and derive it from that on pop.

**Hot path (alloc):**

```c
void* kmalloc_fast(size_t sz) {
    int cls = class_of(sz);          // compile-time for literals
    if (cls >= SLAB_MAG_CLASSES) return kmalloc_slow(sz);

    preempt_disable();
    cpu_t* c = this_cpu();
    if (LIKELY(c->mag.count[cls] > 0)) {
        void* p = c->mag.stack[cls][--c->mag.count[cls]];
        preempt_enable();
        return p;
    }
    preempt_enable();
    return kmalloc_slow_refill(cls);
}
```

On -O2 with ALWAYS_INLINE, this compiles to:

```
    incl   [this_cpu->preempt_depth]     ; preempt_disable
    mov    eax, [this_cpu->mag.count + cls*4]
    test   eax, eax
    jz     slow
    dec    eax
    mov    [this_cpu->mag.count + cls*4], eax
    mov    rax, [this_cpu->mag.stack + cls*SLAB_MAG_DEPTH*8 + rax*8]
    decl   [this_cpu->preempt_depth]     ; preempt_enable
    jne    .ret                            ; skip sched_preempt if depth > 0
    call   sched_preempt
.ret:
    ret
```

That's 9 instructions, no atomics, no locks. ~6-8 cycles on modern
Intel. Compare with the current `pmm_slab_alloc` ~25 cycles.

**Slow path (refill):**

```c
static NOINLINE void* kmalloc_slow_refill(int cls) {
    // Allocate a whole batch from the depot with preempt enabled.
    // SLAB_MAG_REFILL = 16.
    void* batch[SLAB_MAG_REFILL];
    int n = 0;
    for (int i = 0; i < SLAB_MAG_REFILL; i++) {
        void* p = pmm_slab_alloc(&g_kheap.caches[cls]);
        if (!p) break;
        batch[n++] = p;
    }
    if (n == 0) return NULL;

    // One preempt critical section: install the batch, return one.
    preempt_disable();
    cpu_t* c = this_cpu();
    void* result = batch[--n];   // keep one for the caller
    // Copy the rest into the magazine, respecting the depth cap.
    int space = SLAB_MAG_DEPTH - c->mag.count[cls];
    int pushed = (n < space) ? n : space;
    for (int i = 0; i < pushed; i++)
        c->mag.stack[cls][c->mag.count[cls]++] = batch[--n];
    preempt_enable();

    // Leftover batch elements (if the magazine was partially full)
    // go straight back to the depot.  Rare.
    while (n > 0) pmm_slab_free(batch[--n]);
    return result;
}
```

One preempt pair for the entire refill. Depot is called with preempt
enabled so it can block if the underlying PMM needs to allocate a new
slab (which can trigger a buddy alloc that takes the PMM lock).

**kfree path:**

```c
void kfree_fast(void* p) {
    if (!p) return;
    if (!pmm_is_slab_ptr(p)) { kfree_slow(p); return; }

    int cls = class_of(pmm_slab_size_of(p));
    preempt_disable();
    cpu_t* c = this_cpu();
    if (LIKELY(c->mag.count[cls] < SLAB_MAG_DEPTH)) {
        c->mag.stack[cls][c->mag.count[cls]++] = p;
        preempt_enable();
        return;
    }
    preempt_enable();
    kfree_slow_drain(cls, p);
}
```

The `pmm_slab_size_of` call is 3 memory loads (phys→frame→tracker→
slot_size). Acceptable cost — still under 10 cycles total. If it shows
up hot, we can store the class in the 2 high bits of the magazine
stack pointer (cls is 0–9, fits in 4 bits, pointers are 48-bit
canonical) and avoid the tracker lookup entirely.

**Drain path (overflow):**

```c
static NOINLINE void kfree_slow_drain(int cls, void* p) {
    void* batch[SLAB_MAG_REFILL + 1];
    int n = 0;

    preempt_disable();
    cpu_t* c = this_cpu();
    // Drain half the magazine into a stack batch.
    int take = SLAB_MAG_REFILL;
    if (take > (int)c->mag.count[cls]) take = c->mag.count[cls];
    for (int i = 0; i < take; i++)
        batch[n++] = c->mag.stack[cls][--c->mag.count[cls]];
    // Also push the new free pointer if there's space.
    if (c->mag.count[cls] < SLAB_MAG_DEPTH) {
        c->mag.stack[cls][c->mag.count[cls]++] = p;
        p = NULL;
    }
    preempt_enable();

    // Return the drained batch to the depot.
    for (int i = 0; i < n; i++) pmm_slab_free(batch[i]);
    if (p) pmm_slab_free(p);   // magazine was still full after drain
}
```

### Per-CPU pageset for pmm_buddy_alloc(0) (redesigned)

Same idea as the slab magazine, for order-0 physical pages:

```c
typedef struct {
    uint64_t pages[PCP_DEPTH];   // PCP_DEPTH = 64
    uint32_t count;
} pmm_pcp_t;
```

Hot path for `pmm_buddy_alloc(0)`:

```c
phys_addr_t pmm_buddy_alloc(uint8_t order) {
    if (order != 0) return pmm_buddy_alloc_large(order);

    preempt_disable();
    cpu_t* c = this_cpu();
    if (LIKELY(c->pcp.count > 0)) {
        phys_addr_t p = c->pcp.pages[--c->pcp.count];
        preempt_enable();
        return p;
    }
    preempt_enable();
    return pmm_pcp_refill();
}
```

Refill pulls `PCP_REFILL = 16` pages from the buddy allocator in one
held preempt critical section (the buddy allocator does take the PMM
lock, but that's fine — we'll release preempt first if needed).

### Bypass rules (unchanged from reverted attempt)

- `kmalloc(size > 4096)` bypasses the magazine, goes straight to
  `pmm_buddy_alloc(order)` + 16-byte header.
- `pmm_buddy_alloc(order > 0)` bypasses the pcp, goes straight to
  the underlying buddy allocator.
- Freeing a pointer that was allocated via the bypass path skips the
  magazine/pcp and returns directly.

### Counters (must be implemented)

```c
// In cpu_t:
uint64_t mag_hits[SLAB_MAG_CLASSES];
uint64_t mag_misses[SLAB_MAG_CLASSES];
uint64_t mag_drains[SLAB_MAG_CLASSES];
uint64_t pcp_hits, pcp_misses;
```

Incremented non-atomically by the owning CPU (no cross-CPU synchronization).
A debug syscall dumps them so we can verify hit rate > 95% before
declaring Phase 4 done.

### Acceptance criteria for the retry

1. `kmalloc(64)` measured in cycles is less than half of the current
   `pmm_slab_alloc` cost.
2. On a bash+doom+makaterm workload, fast-path hit rate for sizes
   8/16/32/64/128/256/512 exceeds 95%.
3. Display server startup time is not worse than pre-Phase-4 baseline.
4. No new deadlocks, no regressions in `docs/SMP_KNOWN_RACES.md`.

If any of these fail, revert and try something else.

## Bridge to the real thing: the global `g_pmm_lock`

Until the redesign lands, SMP correctness is provided by a single
global spinlock `g_pmm_lock` in `kernel/mm/pmm.c`. Every public PMM
entry takes it with `spin_lock_irqsave` for the duration of its
critical section. This includes:

- `pmm_buddy_alloc` / `pmm_buddy_free`
- `pmm_slab_alloc` / `pmm_slab_free`
- `pmm_ref_inc` / `pmm_ref_dec`
- `pmm_pin` / `pmm_unpin`

Read-only queries (`pmm_ref_get`, `pmm_pin_get`, `pmm_is_slab_ptr`)
do single aligned loads without the lock — they can race with a
concurrent mutation but each individual read is intact.

The internal `_locked` variants (e.g. `pmm_buddy_alloc_locked`) are
used by helpers that are already inside the critical section
(`pmm_slab_grow_locked` calling `pmm_buddy_alloc_locked`). Never
call a non-`_locked` entry from within the lock — it would self-
deadlock.

Performance impact today: one `lock cmpxchg` + `pushfq/popfq` per
alloc/free, ~30 cycles of overhead. On a single CPU this is noticeable
only at microbenchmark scale; normal kernel workloads don't notice.

When the magazine redesign lands, `g_pmm_lock` stays — it becomes the
depot lock behind the per-CPU magazines, and it's only hit on refill
(1 in ~16 allocs after warmup). Its existence remains but its
contention drops to near zero.
