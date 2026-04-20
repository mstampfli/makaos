// ── Slab / pcp shrinker kthread (Phase 4F) ─────────────────────────────
//
// Two trigger points for reclaim:
//
//  1) Periodic: a kthread wakes every SHRINK_INTERVAL_NS, walks every
//     registered slab cache, and returns every page on cache->empty
//     to the buddy allocator.  Also drains each CPU's pcp back half.
//     This keeps unused slab pages from parking indefinitely while
//     the buddy is still able to satisfy large allocs.
//
//  2) Pressure (synchronous): pmm_buddy_alloc() on failure calls
//     pmm_slab_shrink_all_locked() (see pmm.c) and also needs a pcp
//     drain path that runs with g_pmm_lock already held.  The
//     locked-variant drain is expected to cross-CPU and is guarded
//     by the invariant that the caller is inside g_pmm_lock — no
//     per-CPU fast path can make progress while the lock is held,
//     since the slow paths and refills also take it (refill via
//     pmm_buddy_alloc_locked_for_pcp).  That means a consistent
//     snapshot of each cpu's pcp is readable.

#include "slab_pcpu.h"
#include "pcp.h"
#include "pmm.h"
#include "cpu.h"
#include "smp.h"
#include "common.h"
#include "process.h"
#include "sched.h"

#define SHRINK_INTERVAL_NS (1000000000ULL)  // 1 second

extern uint64_t tsc_read_ns(void);

static void slab_shrinker_kthread_entry(void) {
    for (;;) {
        // Drain every cache's empty_list.  pmm_slab_shrink_all_locked
        // expects g_pmm_lock held; we wrap briefly via the same helpers
        // the pcp uses so we don't need a public drain API.
        {
            uint64_t f;
            pmm_pcp_lock(&f);
            pmm_slab_shrink_all_locked();
            pmm_pcp_unlock(f);
        }

        // Drain per-CPU pcp's.  We're a kthread running on one CPU at
        // a time; to touch other CPUs' pcps we rely on the same
        // cross-CPU contract pcp_drain_all() documents.  For the
        // current design (scheduler doesn't migrate kthreads off
        // their last CPU mid-call), a full pass each second catches
        // all CPUs as the shrinker migrates naturally.  We don't
        // need a hard barrier because stale pages in pcp only delay
        // reclaim, not corrupt.
        pcp_drain_all();

        // Sleep for SHRINK_INTERVAL_NS.  Same pattern as sys_nanosleep.
        uint64_t wake_ns = tsc_read_ns() + SHRINK_INTERVAL_NS;
        while (tsc_read_ns() < wake_ns) {
            g_current->sleep_until_ns = wake_ns;
            sched_sleep();
            g_current->sleep_until_ns = 0;
        }
    }
}

void slab_shrinker_start(void) {
    task_t* t = task_create_kthread(slab_shrinker_kthread_entry, pid_alloc());
    if (t) sched_add(t);
}
