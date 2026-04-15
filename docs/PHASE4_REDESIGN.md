# Phase 4 — Per-CPU allocators (deferred)

## Status

**Deferred until after Phase 9 (AP boot).**

Two redesigns exist in this file:

1. **§ The first redesign** (preempt_disable + per-CPU array stack)
   — documented for historical reference.  It is correct and would
   work, but it has been **superseded** by the second redesign.
2. **§ The 2026 redesign: lockless cmpxchg16b fast path + page-state
   shrinker** — the design we will actually build.  Strictly faster
   on the fast path (no preempt_disable at all) and properly returns
   pages to the buddy allocator under memory pressure via a state
   machine rather than ad-hoc shrinking.

The order is deliberate:

1. **Phase 9 lands first.**  APs come online, IPIs work, real SMP
   pressure exists.  Without that we can't measure magazine sizing
   or hit rate against a realistic workload, and the previous
   reverted attempt failed precisely because we tuned in the dark.
2. **Phase 9 also forces a `this_cpu()` refactor** (drop `rdmsr`,
   use `%gs:offset` addressing in inline asm).  That refactor is a
   prerequisite for the cmpxchg16b fast path — doing it now means
   doing it twice.
3. **The existing `g_pmm_lock` SMP-safety bridge stays in place**
   until Phase 4-proper lands.  It's slow but correct.

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

---

# The 2026 redesign: lockless `cmpxchg16b` fast path + page-state shrinker

This section supersedes the preempt_disable design above.  The earlier
design is correct and would work; this one is **strictly faster** and
has none of the cache-eviction concerns the user pushed back on.

## Design goals

The first redesign's hot path was:

```
incl    [this_cpu->preempt_depth]   ; 1 instruction, 1 cycle
mov     eax, [this_cpu->mag.count + cls*4]
test    eax, eax
jz      slow
dec     eax
mov     [this_cpu->mag.count + cls*4], eax
mov     rax, [this_cpu->mag.stack + cls*N*8 + rax*8]
decl    [this_cpu->preempt_depth]   ; 1 instruction, 1 cycle
```

That's ~6 cycles, 9 instructions, two preempt-depth mutations.  Good,
but **not the bound**.  Linux SLUB does it in fewer instructions and
zero preempt mutations using a non-locked `cmpxchg16b` against
`%gs:offset`-relative per-CPU memory.  We will too.

## The Linux SLUB trick (recap)

The per-CPU magazine slot for one size class is a 16-byte aligned
struct of two 8-byte words:

```c
typedef struct {
    void*    freelist;   // head of singly-linked free list of objects
    uint64_t tid;        // monotonically increasing transaction id
} __attribute__((aligned(16))) slab_cpu_slot_t;
```

`tid` is bumped on every successful pop or push.  It is never reset.
Wraparound is harmless because the comparison only ever cares whether
two snapshots taken < ~1 second apart are equal.

The fast-path alloc is:

```
retry:
    snap   = LOAD16 [%gs:slab_cpu_slot[cls]]   ; atomic 16B load
    if (snap.freelist == NULL) goto slow_refill;
    next   = *(snap.freelist)                  ; non-atomic deref
    new    = { next, snap.tid + 1 }
    CMPXCHG16B [%gs:slab_cpu_slot[cls]], snap → new   ; non-locked
    if (failed) goto retry;
    return snap.freelist;
```

Five instructions in the success path.  Roughly 8–10 cycles on
Skylake+.

**Why it's safe across preemption and migration:**

`%gs:offset` is resolved at instruction execution time.  If the task
migrates between the LOAD16 and the CMPXCHG16B, the CMPXCHG16B
operates on the **new** CPU's slot.  The CMPXCHG compares `snap.tid`
against the new CPU's current `tid`.  Two outcomes:

- **Mismatch (the overwhelmingly common case after migration).** The
  CMPXCHG fails, we retry on the new CPU.  Correct.
- **Match (vanishingly rare — would require both CPUs to have
  identical `tid` values, which only happens if migration occurred
  at the very start of an alloc on a freshly-initialized magazine).**
  We pop *that* CPU's freelist head.  Also correct, because we never
  committed anything based on identity of the original CPU.

The non-locked `cmpxchg16b` is atomic with respect to interrupts on
the same core but **not** atomic against other cores.  That's fine
because the memory it touches is per-CPU — only ever written by the
CPU that owns it.

## The hazard the user spotted, and the answer

**Hazard:** between the LOAD16 and the deref `*snap.freelist`, the
slab page that `snap.freelist` points into could be returned to the
buddy allocator and reissued as a network buffer.  The deref then
reads garbage; the CMPXCHG could spuriously succeed and corrupt the
freelist.

**Resolution: page state machine that prevents reclamation while any
pointer is live.**  Pages do not "pin forever" — they return to buddy
under memory pressure — but only when **provably unreachable from any
CPU's fast path**.  The state machine guarantees that.

### Page states

Every slab page lives in exactly one of:

- **`SLAB_CPU_ACTIVE`** — currently the carving page for some CPU's
  size class.  `cpu_t.slab.cpu_slab[cls] == this_page`.  Objects in
  this page may be on `cpu_slot[cls].freelist` and may be popped by
  the lockless fast path.  Cannot be reclaimed.
- **`SLAB_PARTIAL`** — has some free objects, was previously a
  `SLAB_CPU_ACTIVE` for some CPU, was flushed to the cache's
  per-CPU partial list when that CPU's magazine overflowed.  Not
  on any CPU's `cpu_slab[]`.  No fast path can reach it without
  going through the slow refill (which takes the cache lock).
- **`SLAB_EMPTY`** — every object is free.  Lives on the cache's
  empty list.  Reclaimable by the shrinker.

State transitions:

```
NEW PAGE (just allocated from buddy)
   │
   ▼
SLAB_CPU_ACTIVE  ◄────────────────────────────┐
   │     ▲                                     │
   │     └── fast path pops drain the freelist │
   │         to empty; then this CPU swaps     │
   │         in a new SLAB_CPU_ACTIVE          │
   │                                            │
   ▼                                            │
   (overflow / flush via slow path)            │
   │                                            │
   ▼                                            │
SLAB_PARTIAL                                    │
   │                                            │
   │  refill picks a partial page              │
   │  and promotes it to                       │
   ├───────────────────────────────────────────┘
   │
   │  freeing the last in-use object on this page
   ▼
SLAB_EMPTY
   │
   │  shrinker reclaims, page returns to buddy
   ▼
GONE
```

**Critical invariant:** the lockless fast path can only ever
dereference pointers into `SLAB_CPU_ACTIVE` pages of *some* CPU.  It
cannot reach a `SLAB_PARTIAL` page (those are only consulted by the
slow refill under the cache lock) and definitely cannot reach a
`SLAB_EMPTY` page.

The shrinker only reclaims `SLAB_EMPTY` pages.  Therefore: no live
fast-path pointer ever points into a reclaimed page.  No grace
period needed, no fixup table needed, no preempt_disable needed.

### Where the page state lives

We add a per-page metadata word in the existing `pmm_frame_meta_t`
(or equivalent — wherever pmm tracks per-frame info today).  One
byte per frame for the slab state, plus one or two bytes for size
class index and per-page free-object count.

```c
typedef struct {
    uint8_t  slab_state;   // SLAB_CPU_ACTIVE / SLAB_PARTIAL / SLAB_EMPTY / SLAB_NONE
    uint8_t  slab_class;   // size class index, valid if slab_state != NONE
    uint16_t slab_inuse;   // number of allocated objects on this page
} __attribute__((packed)) pmm_slab_pageinfo_t;
```

Lookup is O(1) by physical frame number (already how `pmm_is_slab_ptr`
works today).  A separate side array would also work; the existing
metadata table is the natural home.

## Per-CPU layout

Inside `cpu_t`:

```c
typedef struct {
    // Lockless fast-path slots — one per kmalloc size class.
    // 16-byte aligned so cmpxchg16b is legal.
    slab_cpu_slot_t cpu_slot[KMALLOC_CACHE_COUNT] __attribute__((aligned(16)));

    // The page each cpu_slot[cls].freelist points into.  Used by
    // the slow path to know which page to swap out on overflow.
    struct slab_page* cpu_slab[KMALLOC_CACHE_COUNT];

    // Per-CPU partial list.  When cpu_slab needs to be swapped out
    // and there's no fully-empty refill from the depot, we fall
    // back to this list before going to the global cache.  Reduces
    // global lock contention.
    struct slab_page* partial_head[KMALLOC_CACHE_COUNT];
    uint32_t          partial_count[KMALLOC_CACHE_COUNT];

    // Counters (per-CPU, non-atomic, owner-only writes).
    uint64_t mag_hits[KMALLOC_CACHE_COUNT];
    uint64_t mag_misses[KMALLOC_CACHE_COUNT];
    uint64_t mag_drains[KMALLOC_CACHE_COUNT];
} cpu_slab_state_t;
```

The `cpu_slot[cls]` array is the only thing the lockless fast path
touches.  Everything else is slow-path or counter state.

## Per-cache global state (depot)

```c
typedef struct {
    spinlock_t   lock;              // serialises empty_list / global_partial
    struct slab_page* global_partial;  // partials migrated from CPUs that
                                       // are oversubscribed
    struct slab_page* empty_list;      // shrinker reclaim queue
    uint32_t     empty_count;
    uint32_t     obj_size;
    uint32_t     obj_per_page;
} slab_cache_t;
```

The fast path never touches this.  Only the slow paths
(refill / overflow / shrinker) take `cache->lock`.

## Refill (slow path, fast-path miss)

```c
static void* slab_refill(int cls) {
    cpu_slab_state_t* s = &this_cpu()->slab;
    slab_cache_t*     c = &g_slab_caches[cls];

    // Try this CPU's partial list first — no global lock needed.
    if (s->partial_count[cls] > 0) {
        slab_page_t* page = s->partial_head[cls];
        // ... unlink, install as cpu_slab[cls], publish freelist ...
        return slab_pop_from_freshly_installed_page();
    }

    // Otherwise hit the global cache.
    spin_lock(&c->lock);
    slab_page_t* page = NULL;
    if (c->global_partial) {
        page = c->global_partial;
        c->global_partial = page->next;
    } else if (c->empty_list) {
        page = c->empty_list;
        c->empty_list = page->next;
        c->empty_count--;
    }
    spin_unlock(&c->lock);

    if (!page) {
        // Out of cached pages — buddy alloc.
        phys_addr_t phys = pmm_buddy_alloc(0);
        if (phys == PMM_INVALID_ADDR) return NULL;
        page = slab_init_page(phys, cls);
    }

    // Install as the new cpu_slab and publish its freelist into
    // cpu_slot[cls] via a single 16-byte store.  No CMPXCHG needed
    // here because we are the only writer to our own cpu_slot.
    return slab_install_page(page, cls);
}
```

**Why the per-CPU partial list matters:** without it, every refill
hits `cache->lock`.  With it, common-case refill from a partially-
drained page that this same CPU previously owned is **lockless**.
Linux SLUB does this and it's a measurable win.

## Overflow (slow path, fast-path push that would exceed depth)

When a free pushes to a `cpu_slab` page that's now fully-empty *AND*
this CPU has a different `cpu_slab` already installed, the page
needs to migrate to either the per-CPU partial list (if there's
room) or the global cache:

```c
static void slab_overflow(slab_page_t* page, int cls) {
    cpu_slab_state_t* s = &this_cpu()->slab;

    if (page->slab_inuse == 0) {
        // Fully empty — try to keep on this CPU's partial list.
        if (s->partial_count[cls] < PARTIAL_DEPTH) {
            page->next = s->partial_head[cls];
            s->partial_head[cls] = page;
            s->partial_count[cls]++;
            page->state = SLAB_PARTIAL;
            return;
        }
        // CPU partial list full — promote to global empty list.
        slab_cache_t* c = &g_slab_caches[cls];
        spin_lock(&c->lock);
        page->next = c->empty_list;
        c->empty_list = page;
        c->empty_count++;
        page->state = SLAB_EMPTY;
        spin_unlock(&c->lock);
        return;
    }
    // Still partially used — back to per-CPU partial list.
    // ...
}
```

## The shrinker

A kernel thread (`slab_shrinker_kthread`) runs periodically — say
once per second — and walks every cache:

```c
static void slab_shrinker(void) {
    for (int cls = 0; cls < KMALLOC_CACHE_COUNT; cls++) {
        slab_cache_t* c = &g_slab_caches[cls];
        spin_lock(&c->lock);
        // Move every page on empty_list back to buddy.
        slab_page_t* p = c->empty_list;
        c->empty_list = NULL;
        c->empty_count = 0;
        spin_unlock(&c->lock);
        while (p) {
            slab_page_t* next = p->next;
            phys_addr_t phys = slab_page_to_phys(p);
            // The page metadata is cleared, then phys returns to buddy.
            pmm_slab_pageinfo_clear(phys);
            pmm_buddy_free(phys, 0);
            p = next;
        }
    }
}
```

Two trigger points:

1. **Periodic.** Once per second, even under no pressure, just to
   keep the empty list bounded.
2. **Pressure hook.** If `pmm_buddy_alloc(order)` would otherwise
   return `PMM_INVALID_ADDR`, it first calls `slab_shrink_now()`
   synchronously, then retries.  This is the path that frees up
   slab pages for a large allocation that's about to fail.

**Critical: no `SLAB_CPU_ACTIVE` page is ever on `empty_list`.**  The
state machine prevents it: a page becomes `SLAB_EMPTY` only via
`slab_overflow` after it stops being any CPU's `cpu_slab[cls]`.  By
the time it's on `empty_list`, no CPU has it in its lockless slot,
no fast-path pointer can be reaching into it, and the shrinker is
safe to return it to buddy.

## Per-CPU pageset for `pmm_buddy_alloc(order=0)`

Separate from the slab magazine.  Same idea, simpler:

```c
typedef struct {
    phys_addr_t pages[PCP_DEPTH];   // PCP_DEPTH = 64
    uint32_t    count;
    uint64_t    tid;
} pmm_pcp_t __attribute__((aligned(16)));
```

The fast path is the same cmpxchg16b trick — but easier because
order-0 pages don't have an internal freelist to dereference.  We
just hold an array of phys_addr_t values; pop reads `pages[count-1]`
and decrements `count` via cmpxchg16b on `{count, tid}`.

Refill: take the buddy lock, allocate `PCP_REFILL = 16` order-0
pages, push them all in.  No state machine needed (order-0 pages
either are or aren't in the pcp; no "partial" concept).

For order > 0 buddy allocations: stay on the global buddy lock.
Multi-page allocations are rare and coarse, lock contention there
is acceptable.

## `this_cpu()` refactor (prerequisite for the lockless fast path)

The current implementation uses `rdmsr` on every call:

```c
#define this_cpu() ((cpu_t*)__builtin_ia32_rdmsr(MSR_GS_BASE))
```

`rdmsr` is ~100 cycles.  That kills the fast path before it even
starts.  We need `mov %gs:offset, %reg`-style addressing in C.

Replacement is a set of inline-asm primitives:

```c
#define this_cpu_read(field) ({                       \
    typeof(((cpu_t*)0)->field) __v;                   \
    asm("mov %%gs:%c1, %0" : "=r"(__v)                \
        : "i"(offsetof(cpu_t, field)));               \
    __v;                                              \
})

#define this_cpu_write(field, val) do {               \
    asm("mov %0, %%gs:%c1" :                          \
        : "r"(val), "i"(offsetof(cpu_t, field))       \
        : "memory");                                  \
} while (0)

#define this_cpu_cmpxchg16b(field, old_p, new_p) ({   \
    uint8_t __ok;                                     \
    asm("cmpxchg16b %%gs:%c4 ; setz %0"               \
        : "=q"(__ok),                                 \
          "+a"((old_p)->lo), "+d"((old_p)->hi)        \
        : "b"((new_p)->lo), "c"((new_p)->hi),         \
          "i"(offsetof(cpu_t, field))                 \
        : "memory");                                  \
    __ok;                                             \
})
```

This refactor must land **before or together with** Phase 4-proper.
Doing it during Phase 9 is natural because Phase 9 also overhauls
per-CPU storage.

The existing `this_cpu()` macro stays as a fallback for sites that
need the whole `cpu_t*` (rare).  Hot paths use `this_cpu_read` /
`this_cpu_write` / `this_cpu_cmpxchg16b` exclusively.

## Acceptance criteria (revised)

The retry succeeds only if:

1. `kmalloc(64)` cycle count is below `pmm_slab_alloc`'s current cost
   on a single CPU.  Ideally < 12 cycles.
2. **No preempt_disable on the fast path.**  Period.
3. On a 4-CPU stress workload (4 threads each in a tight
   `kmalloc(N)/kfree(p)` loop, mixed sizes) the fast-path hit rate
   is > 99% and the global cache lock is contended < 1% of allocs.
4. Shrinker correctly returns empty pages to buddy under simulated
   memory pressure (`alloc_large_until_fail` test).
5. `slabtop`-equivalent debug syscall prints per-CPU counters and
   matches alloc/free totals (no leaks).
6. The full `bash + makaterm + display server + doom` workload runs
   indistinguishably from the current `g_pmm_lock` baseline on
   single-CPU, and faster on 2+ CPUs.

## Cross-CPU free

If CPU A frees an object whose backing page is currently the
`SLAB_CPU_ACTIVE` page of CPU B, naive push to A's freelist would
corrupt B's freelist.  Solution: **remote free queue per page**.

Each `slab_page_t` has a small lock-free SPSC queue for cross-CPU
frees.  CPU A's `kfree(p)` does:

1. Identify the owning page via `pmm_slab_page_of(p)`.
2. If the page is in *this* CPU's `cpu_slab[cls]`: lockless fast-path
   push (cmpxchg16b).
3. Else: enqueue `p` on the page's remote_free_queue (one atomic
   xchg or similar).  No CPU sync needed — the next time the
   owning CPU drains its magazine, it sees the queue and absorbs
   the remote frees.

This is cheap because the remote-free path only fires when an
allocation has actually migrated, which is rare in well-behaved
workloads (most objects are freed by their allocating thread).

## Sub-phase breakdown when this is built

1. **4-proper-1:** `this_cpu()` refactor.  Inline-asm primitives.
   No allocator changes.  Re-test, ensure no regressions.
2. **4-proper-2:** Per-page slab state (state machine, no fast-path
   yet).  `pmm_slab_alloc` / `pmm_slab_free` updated to maintain
   `slab_state` and the partial/empty lists.  Still using
   `g_pmm_lock` everywhere.  Verify state transitions in unit-style
   tests.
3. **4-proper-3:** Per-CPU `cpu_slab[cls]` + slow-path refill /
   overflow.  Still preempt_disable on the fast path (for now).
   Verify hit rate counters work.
4. **4-proper-4:** **Lockless cmpxchg16b fast path.**  Replace the
   preempt_disable path.  This is the actual scary commit.
5. **4-proper-5:** Per-CPU pageset for order-0 buddy.
6. **4-proper-6:** Shrinker kthread + buddy pressure hook.
7. **4-proper-7:** Cross-CPU remote-free queue.

Each sub-phase is independently committable and revertible.

## Why this is "perfect" by the user's standard

- **Zero atomics on the alloc/free fast path.**  cmpxchg16b is not
  bus-locked; it's a CPU-local instruction against per-CPU memory.
- **Zero preempt_disable on the fast path.**  Migration-safe by
  construction (`%gs:offset` is resolved at execution time, not at
  C call time).
- **Pages return to buddy under pressure** via the shrinker, so the
  user's "what about large allocs" concern is addressed.  The page
  state machine guarantees correctness — no live fast-path pointer
  can ever reach a reclaimed page.
- **Per-cache and per-CPU sharding** means contention is bounded:
  fast paths never contend with each other, slow paths only contend
  on the depot lock for a single size class, refill contention is
  amortised by `SLAB_MAG_REFILL` (~16-32× per acquire).
- **Linux-grade design.**  Every piece (cmpxchg16b fast path, state
  machine, per-CPU partial list, shrinker, cross-CPU remote-free)
  has a one-to-one mapping to Linux SLUB.  Not a shortcut, not a
  compromise, not a "we'll do it later" — the actual real thing.

## What this does NOT cover

- **NUMA-aware allocation.**  Linux's per-node SLUB structures are
  out of scope.  We have one node.  Add when (if) we have multiple.
- **Object debugging / poisoning / red-zoning.**  Linux's
  `SLUB_DEBUG` infrastructure is huge.  Out of scope; we'll add a
  simple `--debug-slab` flag if we ever need it.
- **Slab merging across size classes.**  Linux merges similarly-
  sized caches to reduce memory footprint.  We have 10 fixed
  classes; merging buys nothing.
- **Adaptive magazine sizing.**  Linux tunes `cpu_slab` size based
  on object size and CPU count.  We start with one fixed depth
  and tune later if measurements demand it.
