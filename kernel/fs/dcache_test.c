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
