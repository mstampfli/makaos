// Phase 8D (kernel-side acceptance test) ───────────────────────────────
//
// Runs from init_kthread after the other self-tests.  Exercises the
// io_uring SQ/CQ pipeline entirely in-kernel: creates a ring, fills
// SQEs directly in the backing memory, calls io_uring_enter_impl,
// drains CQEs and verifies the round-trip.
//
// Skips the "map into user space" step — uses the kernel HHDM aliases
// (uring->sq_hdr, cq_hdr, sqes, cqes) that would otherwise be used
// only by the SQ processor.  This validates the machinery without
// needing a userland spawn + stdout capture dance.

#include "io_uring.h"
#include "common.h"
#include "kprintf.h"
#include "kheap.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"

extern uint64_t tsc_read_ns(void);

// Minimal standalone setup — same as io_uring_create but without
// mapping into user space (we'll read via HHDM).  We still need the
// backing pages and the aliases.
static io_uring_t* test_ring_create(uint32_t entries) {
    uint32_t sq_entries = entries;   // caller passes a power of two
    uint32_t cq_entries = sq_entries * 2;

    uint64_t hdrs = sizeof(io_sq_ring_hdr_t) + sizeof(io_cq_ring_hdr_t);
    uint64_t sqes_off = (hdrs + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
    uint64_t sqes_bytes = sq_entries * sizeof(io_sqe_t);
    uint64_t cqes_off = (sqes_off + sqes_bytes + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
    uint64_t cqes_bytes = cq_entries * sizeof(io_cqe_t);
    uint64_t total = (cqes_off + cqes_bytes + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;

    uint64_t pages = total >> PAGE_SHIFT;
    uint8_t order = 0; while ((1ULL << order) < pages) order++;
    phys_addr_t phys = pmm_buddy_alloc(order);
    if (phys == PMM_INVALID_ADDR) return NULL;

    __builtin_memset((void*)(phys + HHDM_OFFSET), 0, total);

    io_uring_t* u = (io_uring_t*)kmalloc(sizeof(*u));
    if (!u) { pmm_buddy_free(phys, order); return NULL; }
    __builtin_memset(u, 0, sizeof(*u));
    u->backing_phys  = phys;
    u->backing_bytes = total;
    u->sq_entries    = sq_entries;
    u->cq_entries    = cq_entries;
    u->owner_mm      = NULL;      // kernel-only: no user mapping
    u->owner_task    = g_current;
    spin_lock_init(&u->cq_lock);
    wait_queue_init(&u->waitq);
    wait_queue_init(&u->wq_waitq);
    wait_queue_init(&u->sqp_waitq);
    u->worker = NULL;
    u->wq_head = NULL;
    u->worker_stop = 0;
    u->sqp_task = NULL;
    u->sqp_stop = 0;
    u->sqp_idle_ms = 1000;

    uint8_t* kbase = (uint8_t*)(phys + HHDM_OFFSET);
    u->sq_hdr = (io_sq_ring_hdr_t*)(kbase);
    u->cq_hdr = (io_cq_ring_hdr_t*)(kbase + sizeof(io_sq_ring_hdr_t));
    u->sqes   = (io_sqe_t*)(kbase + sqes_off);
    u->cqes   = (io_cqe_t*)(kbase + cqes_off);
    u->sq_hdr->ring_mask    = sq_entries - 1;
    u->sq_hdr->ring_entries = sq_entries;
    u->cq_hdr->ring_mask    = cq_entries - 1;
    u->cq_hdr->ring_entries = cq_entries;
    return u;
}

static void test_ring_destroy(io_uring_t* u) {
    uint64_t pages = u->backing_bytes >> PAGE_SHIFT;
    uint8_t order = 0; while ((1ULL << order) < pages) order++;
    pmm_buddy_free(u->backing_phys, order);
    kfree(u);
}

void io_uring_selftest(void) {
    kprintf("[io_uring_test] starting\n");

    io_uring_t* u = test_ring_create(64);
    if (!u) { kprintf("[io_uring_test] FAILED: ring_create\n"); return; }

    // ── 1. NOP batch ───────────────────────────────────────────
    const uint32_t N = 32;
    for (uint32_t i = 0; i < N; i++) {
        io_sqe_t* s = &u->sqes[(u->sq_hdr->tail + i) & u->sq_hdr->ring_mask];
        s->opcode    = IORING_OP_NOP;
        s->user_data = 0xAAAA0000u + i;
    }
    __atomic_store_n(&u->sq_hdr->tail, u->sq_hdr->tail + N, __ATOMIC_RELEASE);

    uint64_t t0 = tsc_read_ns();
    int submitted = io_uring_enter_impl(u, N, N, IORING_ENTER_GETEVENTS);
    uint64_t t1 = tsc_read_ns();

    if (submitted != (int)N) {
        kprintf("[io_uring_test] FAILED: submitted=%d expected=%u\n",
                submitted, (unsigned)N);
        test_ring_destroy(u); return;
    }

    uint32_t good = 0;
    uint32_t ch = u->cq_hdr->head;
    uint32_t ct = __atomic_load_n(&u->cq_hdr->tail, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < N && ch < ct; i++, ch++) {
        io_cqe_t* c = &u->cqes[ch & u->cq_hdr->ring_mask];
        if (c->res == 0 && c->user_data == (0xAAAA0000u + i)) good++;
    }
    __atomic_store_n(&u->cq_hdr->head, ch, __ATOMIC_RELEASE);

    if (good != N) {
        kprintf("[io_uring_test] FAILED: only %u/%u NOPs returned correct CQE\n",
                (unsigned)good, (unsigned)N);
        test_ring_destroy(u); return;
    }

    uint64_t ns_per = (t1 - t0) / N;
    kprintf("[io_uring_test] NOP batch: %u ops in %lu ns (%lu ns/op)\n",
            (unsigned)N, (uint64_t)(t1 - t0), (uint64_t)ns_per);

    // ── 2. Repeat batches for timing — amortised cost ─────────
    const uint32_t ROUNDS = 100;
    uint64_t total_ns = 0;
    for (uint32_t r = 0; r < ROUNDS; r++) {
        for (uint32_t i = 0; i < N; i++) {
            io_sqe_t* s = &u->sqes[(u->sq_hdr->tail + i) & u->sq_hdr->ring_mask];
            s->opcode    = IORING_OP_NOP;
            s->user_data = 0xBB000000u + r * N + i;
        }
        __atomic_store_n(&u->sq_hdr->tail, u->sq_hdr->tail + N, __ATOMIC_RELEASE);

        uint64_t s0 = tsc_read_ns();
        io_uring_enter_impl(u, N, N, IORING_ENTER_GETEVENTS);
        total_ns += tsc_read_ns() - s0;

        // drain
        uint32_t h2 = u->cq_hdr->head;
        uint32_t t2 = __atomic_load_n(&u->cq_hdr->tail, __ATOMIC_ACQUIRE);
        __atomic_store_n(&u->cq_hdr->head, t2, __ATOMIC_RELEASE);
        (void)h2;
    }
    uint64_t avg_ns_per = total_ns / (ROUNDS * N);
    kprintf("[io_uring_test] steady-state: %u rounds × %u ops, %lu ns/op avg\n",
            (unsigned)ROUNDS, (unsigned)N, (uint64_t)avg_ns_per);

    test_ring_destroy(u);
    kprintf("[io_uring_test] SELF-TEST PASSED\n");
}
