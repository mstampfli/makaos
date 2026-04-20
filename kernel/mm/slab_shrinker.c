// ── Slab / pcp shrinker kthread (Phase 4F + 5D pressure-aware) ─────────
//
// Two trigger points for reclaim:
//
//  1) Periodic kthread:
//       - Reads pmm_free_ratio_bp() (free pages / total, basis points).
//       - Maps ratio → scan policy:
//           >= 7500 (>=75% free)  skip entirely, long sleep
//           5000..7499 (50-74%)   drain oldest 10% per cache, medium sleep
//           2500..4999 (25-49%)   drain 50% per cache, short sleep
//           <  2500 (< 25%)       drain everything, tight loop
//       - pcp_drain_all only runs when ratio < 5000; at high free
//         levels the per-CPU pagesets aren't costing us buddy
//         fragmentation, and draining wastes refill churn.
//
//  2) Pressure (synchronous): pmm_buddy_alloc() failure path still
//     calls pmm_slab_shrink_all_locked() unconditionally — reclaim
//     everything before declaring OOM.  That path is unchanged.
//
// Sleep intervals are picked so idle kernels don't spin the shrinker
// unnecessarily (saves CPU in a data-center setting where memory
// pressure is rare), while memory-starved systems get immediate,
// tight-loop reclaim.

#include "slab_pcpu.h"
#include "pcp.h"
#include "pmm.h"
#include "cpu.h"
#include "smp.h"
#include "common.h"
#include "process.h"
#include "sched.h"
#include "../fs/dcache.h"      // Phase 7D: dcache LRU reclaim hook

#define SHRINK_IDLE_INTERVAL_NS     (5000000000ULL)  // 5 s  — plenty free
#define SHRINK_MODERATE_INTERVAL_NS (1000000000ULL)  // 1 s  — moderate
#define SHRINK_PRESSURE_INTERVAL_NS ( 250000000ULL)  // 250ms — tight
#define SHRINK_EMERGENCY_INTERVAL_NS (50000000ULL)   //  50ms — starving

extern uint64_t tsc_read_ns(void);

static void shrinker_sleep_ns(uint64_t ns) {
    uint64_t wake_ns = tsc_read_ns() + ns;
    while (tsc_read_ns() < wake_ns) {
        g_current->sleep_until_ns = wake_ns;
        sched_sleep();
        g_current->sleep_until_ns = 0;
    }
}

static void slab_shrinker_kthread_entry(void) {
    for (;;) {
        uint32_t ratio_bp = pmm_free_ratio_bp();
        uint64_t sleep_ns;

        if (ratio_bp >= 7500) {
            // Plenty free — don't reclaim, sleep long.
            sleep_ns = SHRINK_IDLE_INTERVAL_NS;
        } else if (ratio_bp >= 5000) {
            // Moderate — drain a few oldest pages per cache.  Keeps
            // empty-lists from growing unbounded while preserving
            // recently-used pages for re-promotion.
            uint64_t f;
            pmm_pcp_lock(&f);
            pmm_slab_shrink_bounded_locked(4);
            pmm_pcp_unlock(f);
            dcache_shrink(32);      // Phase 7D: light dcache trim
            // Phase 7E: trim inode cache leaves idle >= 60s.
            extern uint32_t irtree_shrink(uint32_t, uint64_t);
            irtree_shrink(32, 60ULL * 1000 * 1000 * 1000);
            sleep_ns = SHRINK_MODERATE_INTERVAL_NS;
        } else if (ratio_bp >= 2500) {
            // Notable pressure — drain half the empty list per cache
            // plus drain pcps (free pages currently parked per-CPU).
            uint64_t f;
            pmm_pcp_lock(&f);
            pmm_slab_shrink_bounded_locked(32);
            pmm_pcp_unlock(f);
            pcp_drain_all();
            dcache_shrink(256);     // Phase 7D: moderate dcache trim
            // Phase 7E: trim inode cache leaves idle >= 10s.
            extern uint32_t irtree_shrink(uint32_t, uint64_t);
            irtree_shrink(256, 10ULL * 1000 * 1000 * 1000);
            sleep_ns = SHRINK_PRESSURE_INTERVAL_NS;
        } else {
            // Starving — reclaim everything reclaimable, tight loop.
            uint64_t f;
            pmm_pcp_lock(&f);
            pmm_slab_shrink_all_locked();
            pmm_pcp_unlock(f);
            pcp_drain_all();
            dcache_shrink(UINT32_MAX);   // Phase 7D: reclaim all
                                         // refcount-0 dentries.
            // Phase 7E: reclaim ALL refcount-0 leaves regardless of age.
            extern uint32_t irtree_shrink(uint32_t, uint64_t);
            irtree_shrink(UINT32_MAX, 0);
            sleep_ns = SHRINK_EMERGENCY_INTERVAL_NS;
        }

        shrinker_sleep_ns(sleep_ns);
    }
}

void slab_shrinker_start(void) {
    task_t* t = task_create_kthread(slab_shrinker_kthread_entry, pid_alloc());
    if (t) sched_add(t);
}
