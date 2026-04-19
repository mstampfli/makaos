// chaselev_selftest — stress-test the work-stealing deque on live SMP.
//
// One owner kthread pushes integer "jobs" 1..N into a shared deque
// interleaved with its own pops.  Several thief kthreads on other CPUs
// race to steal from the same deque.  At the end we verify every value
// was consumed exactly once (no duplicates, no losses).  Failure is a
// correctness bug in chaselev.h — halt with a loud diagnostic.

#include "chaselev.h"
#include "kprintf.h"
#include "sched.h"
#include "process.h"
#include "smp.h"

#define CHASELEV_TEST_N        20000u
#define CHASELEV_TEST_THIEVES  3u

// Visible-for-each-thread state.  `bitmap` has one bit per pushed
// value; set when consumed (push values are 1..N, bit = v-1).
static chaselev_deque_t      s_test_dq __attribute__((aligned(64)));
static volatile uint32_t     s_test_bitmap[(CHASELEV_TEST_N + 31) / 32];
static volatile uint32_t     s_test_dup_hits;      // any duplicate consumption
static volatile uint32_t     s_test_owner_done;    // owner signals drain-and-exit
static volatile uint32_t     s_test_thieves_done;  // thieves increment on exit
static volatile uint32_t     s_test_stolen;        // thief-only counter

static inline void test_consume(uint32_t val) {
    uint32_t idx = val - 1;
    uint32_t old = __atomic_fetch_or(&s_test_bitmap[idx >> 5],
                                       1u << (idx & 31),
                                       __ATOMIC_ACQ_REL);
    if (old & (1u << (idx & 31)))
        __atomic_fetch_add(&s_test_dup_hits, 1, __ATOMIC_RELAXED);
}

static void chaselev_thief_thread(void) {
    // Spin-steal until owner says done, then drain, then exit.
    while (!__atomic_load_n(&s_test_owner_done, __ATOMIC_ACQUIRE)) {
        void* v = chaselev_steal(&s_test_dq);
        if (v == CHASELEV_ABORT) { cpu_relax(); continue; }
        if (v) {
            test_consume((uint32_t)(uintptr_t)v);
            __atomic_fetch_add(&s_test_stolen, 1, __ATOMIC_RELAXED);
        } else {
            cpu_relax();
        }
    }
    // Drain the tail.
    for (;;) {
        void* v = chaselev_steal(&s_test_dq);
        if (v == CHASELEV_ABORT) { cpu_relax(); continue; }
        if (!v) break;
        test_consume((uint32_t)(uintptr_t)v);
        __atomic_fetch_add(&s_test_stolen, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&s_test_thieves_done, 1, __ATOMIC_RELEASE);
    g_current->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

void chaselev_selftest(void) {
    kprintf("[chaselev] self-test starting: N=%u thieves=%u cap=%u\n",
            CHASELEV_TEST_N, CHASELEV_TEST_THIEVES, CHASELEV_CAP);

    // Reset state (the deque BSS-zeroes, bitmap too).
    __atomic_store_n(&s_test_owner_done, 0, __ATOMIC_RELEASE);

    // Spawn thieves on their own kthreads.  The scheduler places them
    // on CPUs via pick_home_cpu (round-robin), so if we have >=2 CPUs
    // at least one thief lands on a remote CPU.
    for (uint32_t i = 0; i < CHASELEV_TEST_THIEVES; i++) {
        task_t* t = task_create_kthread(chaselev_thief_thread, pid_alloc());
        if (t) sched_add(t);
    }

    // Owner: push 8, pop 2, loop until N pushed.  Net +6 per cycle
    // keeps the deque non-empty most of the time so thieves have work.
    uint32_t pushed = 0, popped = 0;
    uint32_t next_val = 1;
    while (pushed < CHASELEV_TEST_N) {
        // Push up to 8.
        for (int i = 0; i < 8 && pushed < CHASELEV_TEST_N; i++) {
            if (!chaselev_push(&s_test_dq, (void*)(uintptr_t)next_val)) {
                break;  // full — pop some below to make room
            }
            next_val++;
            pushed++;
        }
        // Pop 2.
        for (int i = 0; i < 2; i++) {
            void* v = chaselev_pop(&s_test_dq);
            if (!v) break;
            test_consume((uint32_t)(uintptr_t)v);
            popped++;
        }
    }

    // Signal thieves to finish draining.
    __atomic_store_n(&s_test_owner_done, 1, __ATOMIC_RELEASE);

    // Drain whatever thieves leave for us.
    for (;;) {
        void* v = chaselev_pop(&s_test_dq);
        if (!v) break;
        test_consume((uint32_t)(uintptr_t)v);
        popped++;
    }

    // Wait for thieves to exit cleanly.
    while (__atomic_load_n(&s_test_thieves_done, __ATOMIC_ACQUIRE)
            != CHASELEV_TEST_THIEVES)
        cpu_relax();

    // Verify: every value appears exactly once in the bitmap.
    uint32_t found = 0;
    for (uint32_t v = 0; v < CHASELEV_TEST_N; v++) {
        if (s_test_bitmap[v >> 5] & (1u << (v & 31))) found++;
    }

    uint32_t stolen = __atomic_load_n(&s_test_stolen, __ATOMIC_ACQUIRE);
    uint32_t dups   = __atomic_load_n(&s_test_dup_hits, __ATOMIC_ACQUIRE);

    kprintf("[chaselev] pushed=%u owner_popped=%u thief_stolen=%u "
            "found=%u missing=%u duplicates=%u\n",
            pushed, popped, stolen, found,
            CHASELEV_TEST_N - found, dups);

    if (found == CHASELEV_TEST_N && dups == 0 && pushed == CHASELEV_TEST_N) {
        kprintf("[chaselev] SELF-TEST PASSED\n");
    } else {
        kprintf("[chaselev] SELF-TEST FAILED\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
}
