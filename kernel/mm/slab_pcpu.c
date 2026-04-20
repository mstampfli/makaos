// ── Per-CPU SLUB-style slab fast path (Phase 4 / 4D) ───────────────────
//
// Two operations are exposed to kheap.c:
//
//   slab_pcpu_alloc(cls)  — returns a fresh object, hits the lockless
//                            cmpxchg16b fast path on cpu_slot[cls]
//   slab_pcpu_free(ptr)   — pushes an object back, fast-path if it
//                            belongs to THIS CPU's cpu_slab[cls],
//                            cross-CPU remote-free otherwise
//
// Migration safety, page-state machine invariants, and the load-bearing
// claim about why the freelist deref is safe are all documented in
// slab_pcpu.h.  Read that comment block first.
//
// SLOW-PATH STRUCTURE (refill):
//   Drains current cpu_slab's remote_free → if it produced objects,
//   republishes the freelist and returns.  Otherwise demotes the
//   current cpu_slab to the appropriate cache list (FULL or EMPTY)
//   and acquires a fresh page from:
//     1) per-CPU partial_head[cls]   (no lock)
//     2) cache->partial              (g_pmm_lock)
//     3) cache->empty                (g_pmm_lock)
//     4) buddy via pmm_slab_grow     (g_pmm_lock + may shrink)
//   The new page is drained of any pending remote_frees, then promoted
//   to CPU_ACTIVE and its freelist transferred into cpu_slot[cls].

#include "slab_pcpu.h"
#include "kheap.h"
#include "cpu.h"
#include "preempt.h"
#include "pmm.h"
#include "common.h"

// kheap.c keeps g_kheap as static; expose it via a getter to avoid
// piercing its TU.  See kheap.c bottom for the implementation.
extern slab_cache_t* kheap_cache_get(uint8_t cls);

// Compile-time invariants.
_Static_assert(SLAB_PCPU_CLASSES == KMALLOC_CACHE_COUNT,
               "SLAB_PCPU_CLASSES must match KMALLOC_CACHE_COUNT");
_Static_assert(sizeof(slab_cpu_slot_t) == 16,
               "slab_cpu_slot_t must be exactly 16 bytes for cmpxchg16b");
_Static_assert(__builtin_offsetof(cpu_t, cpu_slot) % 16 == 0,
               "cpu_slot[] must be 16-byte aligned for cmpxchg16b");

// Constant byte offset of cpu_slot[cls] within cpu_t.  cls is runtime
// but the slot stride is constant, so we resolve to a runtime offset
// using r-constraint addressing (this_cpu_*_at variants).
ALWAYS_INLINE uint64_t cpu_slot_off(uint8_t cls) {
    return __builtin_offsetof(cpu_t, cpu_slot[0])
         + (uint64_t)cls * sizeof(slab_cpu_slot_t);
}

ALWAYS_INLINE uint64_t cpu_slab_off(uint8_t cls) {
    return __builtin_offsetof(cpu_t, cpu_slab[0])
         + (uint64_t)cls * sizeof(void*);
}

// Owner-only publish of {freelist, tid+1} into cpu_slot[cls] using a
// cmpxchg16b retry.  Called from the refill slow path under
// preempt_disable, so the only racers are IRQ-time fast paths on
// THIS CPU; a tid bump invalidates their CAS and they retry.
static void publish_freelist(uint8_t cls, void* fl) {
    uint64_t off = cpu_slot_off(cls);
    uint64_t old_lo, old_hi;
    this_cpu_load16b_at(off, &old_lo, &old_hi);
    while (!this_cpu_cmpxchg16b_at(off, &old_lo, &old_hi,
                                   (uint64_t)fl, old_hi + 1)) {
        // CAS failed: some IRQ-time fast path bumped tid.  Reload and
        // retry.  We're the sole writer modulo IRQ races, so this
        // converges in ≤1 retry under any realistic interrupt rate.
    }
}

// Save+disable IRQs (cli) and return saved flags.  Used to bracket the
// refill slow path: preempt_disable alone allows IRQ re-entry which
// could call kmalloc → slab_refill → corrupt cpu_slab[cls].  The fast
// path doesn't need this — its CAS-tid invariant handles re-entry.
ALWAYS_INLINE uint64_t local_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
ALWAYS_INLINE void local_irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

// Slow path entered when cpu_slot[cls].freelist == NULL on alloc.
// May install a fresh CPU_ACTIVE page; returns one object on success,
// NULL on OOM.
//
// IRQs are disabled for the duration: re-entering refill from an
// interrupt handler would corrupt cpu_slab[cls] (we'd install two
// different pages and lose the first).  IRQ disable on a single CPU
// is one cli/sti pair (~10 cycles); cheaper than any other re-entry
// guard and cleaner than detecting and unwinding a recursive refill.
static NOINLINE void* slab_refill(uint8_t cls) {
    uint64_t flags = local_irq_save();
    cpu_t*        c     = this_cpu();
    slab_cache_t* cache = kheap_cache_get(cls);

    // Steps 1 + 2 (Phase 4G): rescue or demote.
    //
    // Under g_pmm_lock (via pmm_slab_demote_or_keep) we atomically
    // snapshot active->freelist — the chain that cross-CPU frees
    // accumulated via pmm_slab_free_locked while this CPU owned the
    // page — and either:
    //   (a) keep the page CPU_ACTIVE and republish the rescued
    //       freelist into cpu_slot, OR
    //   (b) park the page onto cache->full (nothing to rescue).
    // The lock closes the race window where a concurrent remote
    // free could push to active->freelist between our check and our
    // state change.
    //
    // page->inuse is maintained by pmm_slab_free_locked on cross-CPU
    // frees, so demoted pages have correct inuse for the slow path
    // to drive FULL→PARTIAL→EMPTY transitions.
    slab_header_t* active = (slab_header_t*)c->cpu_slab[cls];
    if (active) {
        void* fl = NULL;
        int kept = pmm_slab_demote_or_keep(cache, active, &fl);
        if (kept) {
            publish_freelist(cls, fl);
            c->slab_mag_misses[cls]++;
            local_irq_restore(flags);
            return slab_pcpu_alloc(cls);    // retry fast path
        }
        c->cpu_slab[cls] = NULL;            // parked to cache->full
    }

    // Step 3: acquire a fresh page.
    slab_header_t* page = NULL;

    // 3a: per-CPU partial list (no lock — owner-only access).  This
    // is the hot slow path on subsequent refills after a batch grab.
    if (c->partial_head[cls]) {
        page = (slab_header_t*)c->partial_head[cls];
        c->partial_head[cls] = page->next;
        if (page->next) ((slab_header_t*)page->next)->prev = NULL;
        page->prev = NULL;
        page->next = NULL;
        c->partial_count[cls]--;
        page->on_list = SLAB_LIST_NONE;
    }

    // 3b: Phase 5C — batched grab from cache->partial.  Pulls up
    // to cpu_partial_cap pages in ONE g_pmm_lock acquire: one becomes
    // the new CPU_ACTIVE page, the rest go onto this CPU's partial
    // list.  Subsequent refills hit 3a for free, skipping the cache
    // lock entirely until the partial list is drained.  Hot small-
    // object caches (kmalloc-64) end up touching the cache lock
    // roughly once per 13 refills on average.
    if (!page) {
        uint32_t cap = cache->cpu_partial_cap;
        if (cap == 0) cap = 1;
        // Reserve room: never overflow partial_head[cls].  Our cap
        // is already ≤ SLAB_PCPU_PARTIAL_DEPTH but enforce anyway.
        uint32_t room = SLAB_PCPU_PARTIAL_DEPTH
                      - (uint32_t)c->partial_count[cls];
        uint32_t want = cap;
        if (want > room + 1) want = room + 1;     // +1 for the active one
        if (want == 0) want = 1;

        slab_header_t* batch[SLAB_PCPU_PARTIAL_DEPTH + 1];
        uint32_t got = pmm_slab_grab_partial_batch(cache, batch, want);
        if (got > 0) {
            page = batch[0];
            // Stash the rest on per-CPU partial.  Order-preserving
            // push (head-insertion reverses, but consumers pop from
            // head so effective FIFO isn't critical).
            for (uint32_t i = 1; i < got; i++) {
                slab_header_t* h = batch[i];
                h->prev = NULL;
                h->next = (struct slab_header_t*)c->partial_head[cls];
                if (h->next) ((slab_header_t*)h->next)->prev = h;
                c->partial_head[cls] = h;
                c->partial_count[cls]++;
                h->on_list = SLAB_LIST_CPU_PART;
            }
            // Count a batch grab — total pages moved is (got), but
            // we charge only one miss per lock acquire (the
            // amortisation win).  Track separately for acceptance.
            if (got > 1) c->slab_mag_drains[cls]++;  // reuse counter
                                                     // as "batch refill"
        }
    }

    // 3c: cache->empty (recycle a previously-emptied page).
    if (!page) page = pmm_slab_grab_empty(cache);

    // 3d: alloc a fresh slab page from the buddy.  Falls back to
    // synchronous shrink-on-failure inside pmm_buddy_alloc.
    if (!page) page = pmm_slab_grow(cache);

    if (!page) {
        // OOM.  Restore cpu_slot to {NULL, tid+1} so future allocs
        // immediately see "no fast path" and re-enter slow path.
        publish_freelist(cls, NULL);
        local_irq_restore(flags);
        return NULL;
    }

    // Step 4: the page we grabbed was drained of remote_free frees
    // under g_pmm_lock inside pmm_slab_grab_partial / _empty / _grow
    // (Phase 5A).  A tiny window between unlock and here may allow
    // new remote pushes to land; those will be absorbed at the next
    // refill via pmm_slab_demote_or_keep's drain hook.  No explicit
    // drain call needed here.

    // Step 5: promote and publish.  The page becomes CPU_ACTIVE; we
    // transfer the freelist from the page header into cpu_slot.
    //
    // Set h->inuse = obj_per_slab so the legacy pmm_slab_free_locked
    // path (which runs for any slow-path free on this page after
    // demotion) sees a sane baseline.  The lockless fast path won't
    // touch inuse — that's fine, inuse only matters once the page
    // leaves CPU_ACTIVE.  When demoting we leave inuse = obj_per_slab
    // (whole page is "out") so the page parks as FULL, and frees that
    // arrive after demotion correctly walk it through PARTIAL → EMPTY.
    page->on_list = SLAB_LIST_CPU_ACTIVE;
    page->inuse   = cache->obj_per_slab;
    c->cpu_slab[cls] = page;
    void* fl = page->freelist;
    page->freelist = NULL;       // ownership now in cpu_slot

    publish_freelist(cls, fl);

    c->slab_mag_misses[cls]++;
    local_irq_restore(flags);

    // Retry the fast path.  Almost always wins on the first attempt
    // since we just installed a fresh non-empty freelist.
    return slab_pcpu_alloc(cls);
}

void* slab_pcpu_alloc(size_t cls_in) {
    if (cls_in >= SLAB_PCPU_CLASSES) return NULL;
    uint8_t  cls = (uint8_t)cls_in;
    uint64_t off = cpu_slot_off(cls);

    // Lockless fast-path: cmpxchg16b pop.
    //
    // LOAD16 cpu_slot[cls] → snap = {freelist, tid}
    // if freelist == NULL: slow refill
    // next = *(void**)freelist           // see slab_pcpu.h migration safety
    // CMPXCHG16B {next, tid+1} → cpu_slot[cls]
    // retry on failure (CAS lost the race or migration changed tid)
    for (;;) {
        uint64_t snap_lo, snap_hi;
        this_cpu_load16b_at(off, &snap_lo, &snap_hi);
        if (snap_lo == 0) return slab_refill(cls);

        void*    next   = *(void**)snap_lo;
        uint64_t new_lo = (uint64_t)next;
        uint64_t new_hi = snap_hi + 1;
        if (this_cpu_cmpxchg16b_at(off, &snap_lo, &snap_hi, new_lo, new_hi)) {
            // Hit — accounting (non-atomic, owner-only).  May land on
            // a different CPU's counter under migration; harmless drift.
            this_cpu()->slab_mag_hits[cls]++;
            return (void*)snap_lo;
        }
        // CAS failed: tid mismatch (migration or interleaved op).  Retry.
    }
}

void slab_pcpu_free(void* ptr) {
    if (!ptr) return;

    slab_header_t* h = pmm_slab_header_of(ptr);
    if (!h) {
        // Caller (kfree) has already routed non-slab pointers to the
        // buddy free path; reaching here means we got passed a wild
        // ptr.  Defensive bail.
        return;
    }
    slab_cache_t* cache = h->cache;
    uint8_t       cls   = cache->class_idx;
    if (cls >= SLAB_PCPU_CLASSES) {
        // Cache isn't kheap-managed (no per-CPU slot).  Use the
        // global locked path.
        pmm_slab_free(ptr);
        return;
    }

    uint64_t slot_off = cpu_slot_off(cls);
    uint64_t slab_off = cpu_slab_off(cls);

    // Fast path: push to cpu_slot if h is THIS CPU's cpu_slab[cls].
    //
    // The cpu_slab read AND the cmpxchg both resolve via %gs at
    // instruction execution, so a migration between them either
    //  (a) leaves the new CPU's cpu_slab also == h (if h happens to
    //      be CPU_ACTIVE on the new CPU too — vanishingly unlikely)
    //  (b) leaves them mismatched, and our cmpxchg fails on tid
    //      → retry; loop re-reads cpu_slab on the new CPU and falls
    //      through to remote_free.
    for (;;) {
        slab_header_t* active = (slab_header_t*)this_cpu_read_ptr_at(slab_off);
        if (active != h) break;

        uint64_t snap_lo, snap_hi;
        this_cpu_load16b_at(slot_off, &snap_lo, &snap_hi);
        *(void**)ptr = (void*)snap_lo;
        if (this_cpu_cmpxchg16b_at(slot_off, &snap_lo, &snap_hi,
                                    (uint64_t)ptr, snap_hi + 1)) {
            this_cpu()->slab_mag_hits[cls]++;
            return;
        }
        // CAS failed: tid mismatch.  Re-read cpu_slab and retry — a
        // genuine migration will fall out of the loop on the next
        // iteration when active != h.
    }

    // Phase 5A: page isn't THIS CPU's cpu_slab — lockless push to
    // the per-page remote_free Treiber stack.  Drained under
    // g_pmm_lock on any slow-path transition that touches this page
    // (pmm_slab_alloc_locked / _free_locked / _destroy_locked /
    //  _demote_or_keep / _grab_*), so items are never lost.
    //
    // Four instructions on the hot path: load h->remote_free, link
    // *ptr = old head, CAS, retry on failure (rare — remote pushes
    // from many CPUs can contend, but chain depth is ~cpus so the
    // CAS succeeds in ≤2 attempts under realistic load).
    //
    // Target cycle count: ~15 (vs ~100 for the locked path we
    // replaced), matching Linux SLUB's cross-CPU free cost.
    void* old = __atomic_load_n(&h->remote_free, __ATOMIC_RELAXED);
    do {
        *(void**)ptr = old;
    } while (!__atomic_compare_exchange_n(&h->remote_free, &old, ptr,
                                            0,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
    this_cpu()->slab_remote_frees[cls]++;
}

void slab_pcpu_init(void) {
    // Stamp class_idx into every kheap-managed cache so the free
    // path can derive cls in O(1).  Other caches (created via
    // pmm_slab_cache_init outside kheap) keep class_idx = 0xFF and
    // route to pmm_slab_free.
    for (uint8_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        slab_cache_t* c = kheap_cache_get(i);
        if (c) c->class_idx = i;
    }
    // Per-CPU state is BSS-zeroed: cpu_slot[*]={NULL,0},
    // cpu_slab[*]=NULL, partial_head[*]=NULL, partial_count[*]=0.
    // First alloc on each (cpu, cls) pair takes the slow refill path
    // and seeds cpu_slot.
}

void slab_pcpu_stats_get(uint32_t cpu, uint32_t cls, slab_pcpu_stats_t* out) {
    if (cpu >= MAX_CPUS || cls >= SLAB_PCPU_CLASSES || !out) return;
    extern cpu_t g_cpus[MAX_CPUS];
    cpu_t* c = &g_cpus[cpu];
    out->hits         = c->slab_mag_hits[cls];
    out->misses       = c->slab_mag_misses[cls];
    out->drains       = c->slab_mag_drains[cls];
    out->remote_frees = c->slab_remote_frees[cls];
}
