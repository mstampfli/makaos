// Work-stealing deque — Chase-Lev algorithm.
//
// Reference: Le, Pop, Cohen, Zappa Nardelli, "Correct and Efficient
// Work-Stealing for Weak Memory Models", PPoPP 2013.  The authors
// derived the exact fences that every major runtime (Go, Tokio, TBB,
// OpenMP) converged on; we use theirs verbatim.
//
// Layout — one owner CPU, zero-or-more thief CPUs:
//
//          +--------+                     +--------+
//   owner  | bottom |                     |  top   |  thieves
//   push/  |        |   <-- slot ring --> |        |  steal
//   pop    +--------+                     +--------+
//
// Semantics:
//   - push (owner):    LIFO from the owner's side, no atomic on the
//                      common path; only the "growing beyond top" check
//                      loads top with acquire.
//   - pop  (owner):    LIFO pop from own side; the degenerate single-
//                      element case races with thieves and is resolved
//                      by a CAS on `top`.
//   - steal (thief):   FIFO from the opposite side; single CAS on `top`.
//
// Why this over the scheduler's existing linked list:
//   - Enqueue/dequeue on the owner is ~3 loads + 1 store (no lock,
//     no atomic RMW on the fast path).  Linked list needs rq_lock.
//   - Thieves don't serialise through the owner's rq_lock — they CAS
//     `top` which the owner only touches on the degenerate pop case.
//   - Works without shared global state across CPUs.
//
// Capacity is fixed at compile time.  A Linux-scale kernel doesn't
// realistically run more than a few hundred ready tasks per priority
// level per CPU; CHASELEV_CAP = 512 gives 3584 tasks per CPU across
// all 7 MLFQ levels before overflow, with a push-returns-0 fallback.
//
// Memory footprint per deque:
//   - 64 B  top   (own cache line)
//   - 64 B  bottom (own cache line)
//   - 4096 B slots
//   - 64-byte alignment
//   ≈ 4.2 KB, so MLFQ_LEVELS * MAX_CPUS * 4.2 KB ≈ 1.9 MB — fine.

#pragma once

#include "common.h"

#define CHASELEV_CAP   512u
#define CHASELEV_MASK  (CHASELEV_CAP - 1u)

_Static_assert((CHASELEV_CAP & CHASELEV_MASK) == 0,
               "CHASELEV_CAP must be a power of two");

typedef struct chaselev_deque {
    // Thief end.  Thieves CAS this; the owner only touches it on the
    // degenerate single-element pop.  Its own cache line so thief
    // traffic doesn't invalidate the owner's hot path.
    volatile uint64_t top;
    uint8_t _pad1[64 - sizeof(uint64_t)];

    // Owner end.  Only the owner writes; only thieves read with acquire.
    volatile uint64_t bottom;
    uint8_t _pad2[64 - sizeof(uint64_t)];

    // Circular buffer of element pointers (typically task_t*).
    void* volatile slots[CHASELEV_CAP];
} __attribute__((aligned(64))) chaselev_deque_t;

// Sentinel returned by chaselev_steal when a thief lost its CAS race
// with another thief (or the owner in the single-element case).  The
// caller can retry or move on — it's NOT the same as "empty" (NULL).
#define CHASELEV_ABORT ((void*)(uintptr_t)-1)

// ── Owner: push a new element (LIFO side) ────────────────────────────
// Returns 1 on success, 0 if full.  Never aborts.
ALWAYS_INLINE int chaselev_push(chaselev_deque_t* q, void* v) {
    uint64_t b = q->bottom;  // owner-local, no atomic needed
    uint64_t t = __atomic_load_n(&q->top, __ATOMIC_ACQUIRE);
    if ((int64_t)(b - t) >= (int64_t)CHASELEV_CAP) return 0;   // full
    q->slots[b & CHASELEV_MASK] = v;
    // Release fence: the slot store must be visible before the
    // bottom-bump a thief will acquire-load and follow.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&q->bottom, b + 1, __ATOMIC_RELAXED);
    return 1;
}

// ── Owner: pop from the bottom (LIFO side) ───────────────────────────
// Returns the element, or NULL if empty.  The single-element race with
// a thief is resolved by CAS on top.
ALWAYS_INLINE void* chaselev_pop(chaselev_deque_t* q) {
    uint64_t b = q->bottom - 1;
    __atomic_store_n(&q->bottom, b, __ATOMIC_RELAXED);
    // SEQ_CST fence: our bottom decrement must be globally ordered
    // before our top read.  Without this, a thief that CAS'd top while
    // we were decrementing could be invisible to us — we'd take the
    // element and so would they.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t t = __atomic_load_n(&q->top, __ATOMIC_RELAXED);
    void* v = NULL;
    if ((int64_t)(t - b) <= 0) {
        // Non-empty — take the element.
        v = q->slots[b & CHASELEV_MASK];
        if (t == b) {
            // Degenerate: one element left and a thief may also be
            // going for it.  CAS top; loser returns empty.
            if (!__atomic_compare_exchange_n(&q->top, &t, t + 1,
                                               0, __ATOMIC_SEQ_CST,
                                               __ATOMIC_RELAXED))
                v = NULL;
            // Whether we won or lost, deque is now logically empty.
            __atomic_store_n(&q->bottom, b + 1, __ATOMIC_RELAXED);
        }
    } else {
        // Empty — undo the bottom decrement.
        __atomic_store_n(&q->bottom, b + 1, __ATOMIC_RELAXED);
    }
    return v;
}

// ── Thief: steal from the top (FIFO side) ─────────────────────────────
// Returns the element, NULL on empty, or CHASELEV_ABORT on CAS loss.
ALWAYS_INLINE void* chaselev_steal(chaselev_deque_t* q) {
    uint64_t t = __atomic_load_n(&q->top, __ATOMIC_ACQUIRE);
    // SEQ_CST fence: symmetry with the owner's pop — our top read and
    // our bottom read must be globally ordered so we see a consistent
    // snapshot.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t b = __atomic_load_n(&q->bottom, __ATOMIC_ACQUIRE);
    if ((int64_t)(t - b) >= 0) return NULL;                // empty
    void* v = q->slots[t & CHASELEV_MASK];
    // CAS top — loser retries (or moves on to a different victim).
    if (!__atomic_compare_exchange_n(&q->top, &t, t + 1,
                                       0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_RELAXED))
        return CHASELEV_ABORT;
    return v;
}

// ── Advisory: approximate size ────────────────────────────────────────
// Read-only, no synchronization.  Used by stealing policy to skip
// near-empty victims; imprecise-but-monotonic enough for that purpose.
ALWAYS_INLINE uint32_t chaselev_size_approx(chaselev_deque_t* q) {
    uint64_t b = __atomic_load_n(&q->bottom, __ATOMIC_RELAXED);
    uint64_t t = __atomic_load_n(&q->top, __ATOMIC_RELAXED);
    int64_t d = (int64_t)(b - t);
    return d > 0 ? (uint32_t)d : 0;
}

// Public self-test.  Spawns thieves, hammers push/pop/steal, verifies
// every pushed element is consumed exactly once.  Call from a boot-time
// kthread on the BSP.
void chaselev_selftest(void);
