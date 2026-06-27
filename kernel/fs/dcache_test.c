// ── Phase 7F: dcache acceptance + benchmark self-test ──────────────────
//
// Runs from init_kthread after the typesafe_test passes.  Validates:
//
//   1. Invalidation semantics:
//        - Install a negative dentry for a bogus path.
//        - Invalidate it, verify next lookup misses.
//   2. Lookup cycle savings:
//        - Run N path walks on real /bin paths.
//        - Measure cycles per lookup before + after warm cache.
//        - Assert hit rate >= 80% on the warm round.
//        - Report speedup.

#include "dcache.h"
#include "ext2.h"
#include "common.h"
#include "kprintf.h"
#include "sched.h"
#include "process.h"
#include "smp.h"

extern uint64_t tsc_read_ns(void);

// Paths that exist in the on-disk image.  Keep short so the whole
// test fits in kstack.
static const char* s_test_paths[] = {
    "/bin/login",
    "/bin/svcmgr",
    "/bin/bash",
    "/bin/ls",
    "/bin/cat",
    "/bin/echo",
    "/etc/passwd",
    "/etc/shadow",
    "/etc/services/net.svc",
};
#define DCACHE_TEST_PATH_COUNT 9

void dcache_selftest(void) {
    kprintf("[dcache_test] starting\n");

    // ── Phase 1: invalidation semantics ─────────────────────────
    // Install a negative dentry for (ino=2, "nonexistent_xyz"), then
    // invalidate.  Next lookup must miss.
    const char* name = "__dcache_test_negative__";
    uint32_t nlen = 0; while (name[nlen]) nlen++;
    uint32_t nhash = dcache_name_hash(name, nlen);

    dentry_t* neg = dcache_install(NULL, 2, name, nlen, nhash,
                                     DCACHE_NEGATIVE);
    if (!neg) {
        kprintf("[dcache_test] FAILED: install negative returned NULL\n");
        return;
    }
    dcache_put(neg);

    dentry_t* hit = dcache_lookup(2, name, nlen, nhash);
    if (!hit || hit->child_ino != DCACHE_NEGATIVE) {
        kprintf("[dcache_test] FAILED: negative dentry not cached\n");
        if (hit) dcache_put(hit);
        return;
    }
    dcache_put(hit);

    dcache_invalidate(2, name, nlen);
    dentry_t* post = dcache_lookup(2, name, nlen, nhash);
    if (post) {
        kprintf("[dcache_test] FAILED: invalidate didn't remove dentry\n");
        dcache_put(post);
        return;
    }

    // ── Phase 2: lookup cycle savings ───────────────────────────
    dcache_stats_t st0; dcache_stats_get(&st0);

    // Cold round — likely miss, scans on-disk directories.
    uint64_t cold_ns_start = tsc_read_ns();
    for (uint32_t i = 0; i < DCACHE_TEST_PATH_COUNT; i++) {
        int err = 0;
        (void)ext2_lookup_path(s_test_paths[i], 0, &err);
    }
    uint64_t cold_ns = tsc_read_ns() - cold_ns_start;

    // Warm round — same paths, should hit dcache and skip dir_lookup.
    uint64_t warm_ns_start = tsc_read_ns();
    const uint32_t WARM_ITERS = 1000;
    for (uint32_t r = 0; r < WARM_ITERS; r++) {
        for (uint32_t i = 0; i < DCACHE_TEST_PATH_COUNT; i++) {
            int err = 0;
            (void)ext2_lookup_path(s_test_paths[i], 0, &err);
        }
    }
    uint64_t warm_ns = tsc_read_ns() - warm_ns_start;

    dcache_stats_t st1; dcache_stats_get(&st1);
    uint64_t hits   = st1.hits          - st0.hits;
    uint64_t misses = st1.misses        - st0.misses;
    uint64_t total  = hits + misses;
    uint64_t hit_bp = total ? (hits * 10000) / total : 0;

    // Each cold iter hit the on-disk scan.  Each warm iter should
    // be near-instantaneous (dcache hits only).
    uint64_t total_lookups = WARM_ITERS * DCACHE_TEST_PATH_COUNT;
    uint64_t warm_ns_per   = warm_ns / total_lookups;
    uint64_t cold_ns_per   = cold_ns / DCACHE_TEST_PATH_COUNT;
    uint64_t speedup_10x   = cold_ns_per / (warm_ns_per ? warm_ns_per : 1);

    kprintf("[dcache_test] hits=%lu misses=%lu hit_rate=%lu/10000\n",
            (uint64_t)hits, (uint64_t)misses, (uint64_t)hit_bp);
    kprintf("[dcache_test] cold=%lu ns/lookup warm=%lu ns/lookup speedup=%lux\n",
            (uint64_t)cold_ns_per,
            (uint64_t)warm_ns_per,
            (uint64_t)speedup_10x);
    kprintf("[dcache_test] live=%u table_cap=%u installs=%lu evictions=%lu\n",
            (unsigned)st1.live_count, (unsigned)st1.table_cap,
            (uint64_t)(st1.installs - st0.installs),
            (uint64_t)(st1.evictions - st0.evictions));

    // Correctness gate is the hit rate — a working cache produces
    // >99 % hits on repeated lookups.  Timing output is reported but
    // not asserted on: QEMU host scheduler quantum lands mid-test
    // at random and spikes individual lookups into the ms range,
    // making strict speedup thresholds flaky.  When CSPRNG or other
    // kthreads preempt, warm can even exceed cold.
    if (hit_bp < 8000) {
        kprintf("[dcache_test] FAILED: hit rate %lu/10000 < 8000\n",
                (uint64_t)hit_bp);
        return;
    }
    kprintf("[dcache_test] SELF-TEST PASSED\n");
}

// ── Audit T1: dcache resurrect-from-zero race regression test ───────────
//
// dcache_lookup bumps refcount lock-free; the shrinker frees refcount-0
// dentries.  Before the DCACHE_REF_DYING CAS fix, a lookup could resurrect
// (0 -> 1) a dentry the shrinker had already committed to free from its 0
// snapshot, leaving a freed slot referenced / on the LRU -> use-after-free,
// and (via SLAB_TYPESAFE_BY_RCU slot reuse) a looker could read a stale
// child_ino out of a recycled slot.  This stresses lookers + a shrinker +
// reinstalls over a small shared key set on live SMP and asserts no looked-up
// dentry ever yields a child_ino other than the value it was installed with.

#define DRT_KEYS     16u
#define DRT_ITERS    50000u
#define DRT_LOOKERS  2u
#define DRT_PINO     900000u

static char              s_drt_name[DRT_KEYS][8];
static uint32_t          s_drt_hash[DRT_KEYS];
static volatile uint32_t s_drt_lookers_done;
static volatile uint32_t s_drt_corrupt;
static volatile uint32_t s_drt_stop;

static inline uint32_t drt_cino(uint32_t k) { return 500000u + k; }

static void drt_install_one(uint32_t k) {
    dentry_t* d = dcache_install(NULL, DRT_PINO, s_drt_name[k], 4,
                                 s_drt_hash[k], drt_cino(k));
    if (d) dcache_put(d);
}

static void drt_looker_thread(void) {
    uint32_t seed = (uint32_t)(uintptr_t)g_current | 1u;
    for (uint32_t i = 0; i < DRT_ITERS &&
             !__atomic_load_n(&s_drt_stop, __ATOMIC_ACQUIRE); i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;  // xorshift
        uint32_t k = seed % DRT_KEYS;
        dentry_t* d = dcache_lookup(DRT_PINO, s_drt_name[k], 4, s_drt_hash[k]);
        if (d) {
            uint32_t ci = __atomic_load_n(&d->child_ino, __ATOMIC_RELAXED);
            if (ci != drt_cino(k))
                __atomic_fetch_add(&s_drt_corrupt, 1u, __ATOMIC_RELAXED);
            dcache_put(d);
        } else {
            drt_install_one(k);   // keep the key set populated
        }
    }
    __atomic_fetch_add(&s_drt_lookers_done, 1u, __ATOMIC_RELEASE);
    g_current->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

void dcache_race_selftest(void) {
    kprintf("[dcache_race] starting: %u lookers + shrinker, keys=%u iters=%u\n",
            DRT_LOOKERS, DRT_KEYS, DRT_ITERS);
    __atomic_store_n(&s_drt_lookers_done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_drt_corrupt, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_drt_stop, 0u, __ATOMIC_RELEASE);

    for (uint32_t k = 0; k < DRT_KEYS; k++) {
        s_drt_name[k][0] = 'r'; s_drt_name[k][1] = 't';
        s_drt_name[k][2] = (char)('0' + (k / 10));
        s_drt_name[k][3] = (char)('0' + (k % 10));
        s_drt_name[k][4] = '\0';
        s_drt_hash[k] = dcache_name_hash(s_drt_name[k], 4);
        drt_install_one(k);
    }

    for (uint32_t i = 0; i < DRT_LOOKERS; i++) {
        task_t* t = task_create_kthread(drt_looker_thread, pid_alloc());
        if (t) sched_add(t);
    }

    // This thread hammers the shrinker concurrently with the lookers.
    for (uint32_t i = 0; i < DRT_ITERS; i++) {
        dcache_shrink(4);
        if (__atomic_load_n(&s_drt_lookers_done, __ATOMIC_ACQUIRE) == DRT_LOOKERS)
            break;
    }
    __atomic_store_n(&s_drt_stop, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_drt_lookers_done, __ATOMIC_ACQUIRE) != DRT_LOOKERS)
        cpu_relax();

    uint32_t corrupt = __atomic_load_n(&s_drt_corrupt, __ATOMIC_ACQUIRE);
    dcache_shrink(0xFFFFFFFFu);   // final reclaim — must not fault on a freed slot
    kprintf("[dcache_race] child_ino corruptions observed: %u\n", corrupt);
    if (corrupt == 0) {
        kprintf("[dcache_race] SELF-TEST PASSED (no resurrect-from-zero reuse)\n");
    } else {
        kprintf("[dcache_race] SELF-TEST FAILED (%u corruptions)\n", corrupt);
        for (;;) __asm__ volatile("cli; hlt");
    }
}

// Guard for the F131 fix: dcache_invalidate must NOT free a dentry a path walk
// holds by refcount -- it hash-unlinks (so future lookups miss + re-resolve) but
// leaves the held dentry alive + usable; only the holder's final dcache_put
// drops it to the LRU, where the shrinker reclaims it.  The old code call_rcu-
// freed unconditionally, so the holder's later child_ino read + dcache_put hit
// freed memory.  Single-threaded behavioral check (the cross-CPU freed-write is
// code-proven + boot-exercised): hold a ref across an invalidate and confirm
// future lookups miss yet the held dentry stays intact + puttable.
void dcache_held_invalidate_selftest(void) {
    extern void kprintf(const char*, ...);
    const char* name = "__dcache_held_inval__";
    uint32_t nlen = 0; while (name[nlen]) nlen++;
    uint32_t nhash = dcache_name_hash(name, nlen);
    const uint32_t pino = 0x7A11u, cino = 0xC0FFEEu;
    int fails = 0;

    dentry_t* d = dcache_install(NULL, pino, name, nlen, nhash, cino);
    if (!d) { kprintf("[dcache_held] FAILED: install returned NULL\n"); return; }
    // A path walk holds a SECOND reference (refcount now 2).
    dentry_t* held = dcache_lookup(pino, name, nlen, nhash);
    if (held != d) fails++;

    // Invalidate while held: hash-unlink (lookups miss) but do NOT free d.
    dcache_invalidate(pino, name, nlen);

    dentry_t* post = dcache_lookup(pino, name, nlen, nhash);
    if (post) { fails++; dcache_put(post); }            // future lookups must miss
    if (!held || held->child_ino != cino) fails++;       // held dentry intact + usable

    // Drop both refs -> d goes on the LRU (orphaned), the shrinker reclaims it.
    dcache_put(held);
    dcache_put(d);

    kprintf(fails ? "[dcache_held] SELF-TEST FAILED\n"
                  : "[dcache_held] SELF-TEST PASSED (held dentry survives invalidate, hash-unlinked)\n");
}
