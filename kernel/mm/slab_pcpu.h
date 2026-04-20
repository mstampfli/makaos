#pragma once
#include "common.h"
#include "pmm.h"
#include "kheap.h"

// ── Per-CPU SLUB-style slab fast path (Phase 4) ────────────────────────
//
// This is the kernel's primary kmalloc/kfree path under SMP.  Each CPU
// keeps a 16-byte cmpxchg16b-protected slot per kmalloc size class
// pointing at the head of an intrusive freelist that lives inside the
// currently-bound slab page (the "CPU active page" — Linux SLUB term).
//
// Hot path (alloc):
//   1. Snapshot {freelist, tid} = LOAD16 %gs:cpu_slot[cls]
//   2. If freelist == NULL → slow refill
//   3. next = *(void**)freelist     (deref the head's link word)
//   4. CMPXCHG16B {next, tid+1} → cpu_slot[cls]; retry on mismatch
//   5. Return freelist              (the popped object)
//
// Hot path (free):
//   1. Look up the owning page via pmm_slab_header_of(ptr)
//   2. If the page is THIS CPU's cpu_slab[cls] → cmpxchg16b push
//   3. Otherwise → atomic-xchg push onto the page's remote_free
//      Treiber stack (cross-CPU free; drained by owner on next miss)
//
// Slow path (refill / overflow / install): runs under preempt_disable
// and per-cache spinlock, hits pmm_slab_alloc_locked / pmm_slab_grab_*.
//
// MIGRATION SAFETY (the load-bearing claim):
//   - cmpxchg16b operates on memory at instruction execution time, not
//     at C call time.  If a task migrates between LOAD16 and CMPXCHG16B,
//     the CMPXCHG16B operates on the NEW CPU's slot.  The CAS will fail
//     (different tid) and the caller retries on the new CPU.  Correct.
//   - The dereference *(void**)freelist could in principle touch a freed
//     page if the slab page were reclaimed between LOAD16 and that
//     dereference.  This is prevented by the page-state machine in
//     pmm.c: pages on cache->empty (the only ones the shrinker
//     reclaims) cannot be on any CPU's cpu_slot[cls] — they exit
//     CPU_ACTIVE → PARTIAL → EMPTY through cache lock-protected slow
//     paths that wait for any in-flight fast paths to drain via tid
//     mismatch.  Hence the freelist pointer published in cpu_slot can
//     only point into a CPU_ACTIVE page, which by definition is never
//     reclaimed.

void   slab_pcpu_init(void);     // wires every kheap cache to the fast path

// Public hot-path entry points called by kheap.c kmalloc/kfree.  Returns
// NULL if the size class doesn't fit (caller falls back to buddy).
void*  slab_pcpu_alloc(size_t cls);
void   slab_pcpu_free(void* ptr);   // size class derived from page header

// Stats dump (Phase 4H) — fills user-supplied counters per (cpu, cls).
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t drains;
    uint64_t remote_frees;
} slab_pcpu_stats_t;

void slab_pcpu_stats_get(uint32_t cpu, uint32_t cls, slab_pcpu_stats_t* out);
