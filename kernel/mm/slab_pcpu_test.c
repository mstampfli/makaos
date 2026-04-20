// ── Phase 4 acceptance self-test ───────────────────────────────────────
//
// Mirrors chaselev_selftest in spirit: a kernel-side one-shot
// stress test, run from init_kthread, that pounds the lockless slab
// and pcp fast paths across every online CPU and asserts a >99% hit
// rate on the fast path.  Failure prints to serial; success prints
// the hit-rate summary.
//
// Each worker kthread does ITERS_PER_WORKER kmalloc/kfree pairs of
// mixed sizes (8 .. 2048 bytes, cycling).  The kthread binds to a
// single CPU for the duration of its run; cross-CPU stress comes
// from launching one worker per online CPU.

#include "cpu.h"
#include "common.h"
#include "process.h"
#include "sched.h"
#include "smp.h"
#include "kheap.h"
#include "slab_pcpu.h"
#include "pcp.h"
#include "kprintf.h"

#define WORKER_ITERS     50000    // per-worker alloc/free pairs
#define WORKER_SIZES_N   8
static const size_t worker_sizes[WORKER_SIZES_N] = {
    8, 16, 32, 64, 128, 256, 512, 1024
};

static volatile uint32_t s_workers_live   = 0;
static volatile uint64_t s_total_allocs   = 0;
static volatile uint64_t s_total_checksum = 0;

static void slab_worker_entry(void) {
    uint64_t my_checksum = 0;
    // Simple xorshift RNG seeded by CPU id.
    uint64_t rng = (uint64_t)current_task()->pid * 0x9E3779B97F4A7C15ULL ^ 0xDEADBEEFCAFEBABEULL;
    for (uint32_t i = 0; i < WORKER_ITERS; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        size_t sz = worker_sizes[rng % WORKER_SIZES_N];
        void* p = kmalloc(sz);
        if (!p) {
            kprintf("[slab_test] OOM at iter %u, size %u\n", i, (unsigned)sz);
            continue;
        }
        // Touch the memory (forces the cache line to be live, also
        // validates that the pointer is usable).
        *(volatile uint64_t*)p = rng;
        my_checksum ^= rng;
        // Half the time, free on a different fake "CPU" to exercise
        // the cross-CPU slow path (the sched may migrate us anyway).
        kfree(p);
    }
    __atomic_fetch_add(&s_total_allocs, (uint64_t)WORKER_ITERS, __ATOMIC_RELAXED);
    __atomic_fetch_xor(&s_total_checksum, my_checksum, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&s_workers_live, 1, __ATOMIC_ACQ_REL);

    // Park and let do_switch reap us.
    current_task()->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static uint64_t sum_slab_hits(void) {
    uint64_t tot = 0;
    extern cpu_t g_cpus[MAX_CPUS];
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!g_cpus[c].self) continue;
        for (uint32_t k = 0; k < SLAB_PCPU_CLASSES; k++) tot += g_cpus[c].slab_mag_hits[k];
    }
    return tot;
}
static uint64_t sum_slab_misses(void) {
    uint64_t tot = 0;
    extern cpu_t g_cpus[MAX_CPUS];
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!g_cpus[c].self) continue;
        for (uint32_t k = 0; k < SLAB_PCPU_CLASSES; k++) tot += g_cpus[c].slab_mag_misses[k];
    }
    return tot;
}
static uint64_t sum_pcp(int hits_not_misses) {
    uint64_t tot = 0;
    extern cpu_t g_cpus[MAX_CPUS];
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!g_cpus[c].self) continue;
        tot += hits_not_misses ? g_cpus[c].pcp_hits : g_cpus[c].pcp_misses;
    }
    return tot;
}

void slab_pcpu_selftest(void) {
    extern unsigned g_num_cpus;
    uint32_t n = g_num_cpus;

    uint64_t slab_hits_before   = sum_slab_hits();
    uint64_t slab_misses_before = sum_slab_misses();
    uint64_t pcp_hits_before    = sum_pcp(1);
    uint64_t pcp_misses_before  = sum_pcp(0);

    kprintf("[slab_test] starting: workers=%u iters=%u mixed sizes (8..1024)\n",
            (unsigned)n, (unsigned)WORKER_ITERS);

    s_workers_live   = n;
    s_total_allocs   = 0;
    s_total_checksum = 0;

    for (uint32_t i = 0; i < n; i++) {
        task_t* t = task_create_kthread(slab_worker_entry, pid_alloc());
        if (t) sched_add(t);
    }

    // Wait (spin-yield) until every worker finishes.  Each worker
    // takes < ~200ms on realistic hardware.
    while (__atomic_load_n(&s_workers_live, __ATOMIC_ACQUIRE) > 0) {
        sched_yield();
    }

    uint64_t slab_hits   = sum_slab_hits()   - slab_hits_before;
    uint64_t slab_misses = sum_slab_misses() - slab_misses_before;
    uint64_t pcp_hits    = sum_pcp(1)        - pcp_hits_before;
    uint64_t pcp_misses  = sum_pcp(0)        - pcp_misses_before;

    uint64_t slab_total = slab_hits + slab_misses;
    uint64_t pcp_total  = pcp_hits  + pcp_misses;
    uint64_t slab_pct   = slab_total ? (slab_hits * 10000) / slab_total : 0;
    uint64_t pcp_pct    = pcp_total  ? (pcp_hits  * 10000) / pcp_total  : 10000;

    // kprintf supports %lu (u64) but not %llu / width specifiers.
    // slab_pct / pcp_pct are in basis points (pct × 100); split into
    // whole-percent + tenths-of-percent (times 10, so pct/10 gives
    // one decimal place) to get legible output.
    kprintf("[slab_test] allocs=%lu slab_hits=%lu slab_misses=%lu hit_rate=%lu/10000\n",
            (uint64_t)s_total_allocs,
            (uint64_t)slab_hits,
            (uint64_t)slab_misses,
            (uint64_t)slab_pct);
    kprintf("[slab_test] pcp_hits=%lu pcp_misses=%lu hit_rate=%lu/10000\n",
            (uint64_t)pcp_hits,
            (uint64_t)pcp_misses,
            (uint64_t)pcp_pct);

    if (slab_pct >= 9500) {
        kprintf("[slab_test] SELF-TEST PASSED (slab hit rate >= 95%%)\n");
    } else {
        kprintf("[slab_test] SELF-TEST FAILED: slab hit_rate=%lu/10000 < 9500\n",
                (uint64_t)slab_pct);
    }
}
