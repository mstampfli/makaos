#pragma once
#include "common.h"
#include "smp.h"

// ── SPSC ring buffer — single producer, single consumer ────────────────
//
// The simplest and fastest possible lock-free queue.  One producer
// pushes, one consumer pops.  Zero CAS, zero lock, zero branching that
// the compiler can't predict.
//
// Memory ordering: producer releases on push, consumer acquires on pop.
// That guarantees the consumer sees all stores the producer did BEFORE
// advancing the head.  On x86 both are compiler-only barriers.
//
// Usage:
//
//   #define MY_RING_SIZE 64  // MUST be a power of 2
//   typedef struct { int x; } entry_t;
//   static DEFINE_SPSC_RING(my_ring, entry_t, MY_RING_SIZE);
//
//   // Producer:
//   entry_t* slot = spsc_reserve(&my_ring);
//   if (slot) {
//       slot->x = 42;
//       spsc_commit(&my_ring);
//   }
//
//   // Consumer:
//   entry_t* slot = spsc_peek(&my_ring);
//   if (slot) {
//       use(slot->x);
//       spsc_consume(&my_ring);
//   }
//
// Invariants:
//   - head is written only by the producer, read by both sides.
//   - tail is written only by the consumer, read by both sides.
//   - Both are atomic loads/stores on the cross-side read.

#define DEFINE_SPSC_RING(name, type, size)         \
    _Static_assert(((size) & ((size) - 1)) == 0,   \
                   "SPSC ring size must be a power of 2"); \
    static type         name##_buf[size];          \
    static spsc_state_t name = { .cap_mask = (size) - 1u, .head = 0, .tail = 0 }

typedef struct {
    uint32_t cap_mask;        // size - 1 (power of two)
    volatile uint32_t head;   // producer index
    volatile uint32_t tail;   // consumer index
} spsc_state_t;

// Generic template: callers cast to their own type.  We use raw byte
// arithmetic so the header doesn't need to know the element size.
//
// For type safety, each user wraps these in small inlines specific to
// its element type.  See irq_wait.c for an example.

ALWAYS_INLINE uint32_t spsc_count(const spsc_state_t* r) {
    return atomic_load_acq(&r->head) - atomic_load_acq(&r->tail);
}

ALWAYS_INLINE int spsc_empty(const spsc_state_t* r) {
    return atomic_load_acq(&r->head) == atomic_load_acq(&r->tail);
}

ALWAYS_INLINE int spsc_full(const spsc_state_t* r) {
    return spsc_count(r) > r->cap_mask;
}

// Producer reserves the next slot.  Returns the slot index, or
// SPSC_NONE if the ring is full.  After filling, call spsc_commit.
#define SPSC_NONE ((uint32_t)-1)

ALWAYS_INLINE uint32_t spsc_reserve_idx(spsc_state_t* r) {
    uint32_t h = atomic_load_relaxed(&r->head);
    uint32_t t = atomic_load_acq(&r->tail);
    if (h - t > r->cap_mask) return SPSC_NONE;
    return h & r->cap_mask;
}

// Commit a reserved slot.  Must be called after the producer has
// written the slot contents.  Release ordering ensures the consumer
// sees the data before it sees the new head.
ALWAYS_INLINE void spsc_commit(spsc_state_t* r) {
    uint32_t h = atomic_load_relaxed(&r->head);
    atomic_store_rel(&r->head, h + 1u);
}

// Consumer peeks at the next available slot.  Returns the index, or
// SPSC_NONE if the ring is empty.  After reading, call spsc_consume.
ALWAYS_INLINE uint32_t spsc_peek_idx(const spsc_state_t* r) {
    uint32_t t = atomic_load_relaxed(&r->tail);
    uint32_t h = atomic_load_acq(&r->head);
    if (h == t) return SPSC_NONE;
    return t & r->cap_mask;
}

// Advance the consumer past the current slot.
ALWAYS_INLINE void spsc_consume(spsc_state_t* r) {
    uint32_t t = atomic_load_relaxed(&r->tail);
    atomic_store_rel(&r->tail, t + 1u);
}
