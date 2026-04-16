#pragma once
//
// ── IPI call infrastructure — public API ────────────────────────────────
//
// VEC_IPI_CALL (0x42) drives the generic "run this function on CPU N"
// path.  Senders stack-allocate an ipi_call_slot_t, push it onto the
// target's per-CPU MPSC queue with smp_call_push, fire the IPI, and
// spin on slot->done.  The target drains its queue in ipi_call_handler
// (see ipi.c), executes each fn(arg), and RELEASE-stores done=1.
//
// This header exposes just enough for external callers (expedited RCU,
// any future cross-CPU kick) without leaking implementation details.

#include "common.h"

typedef struct ipi_call_slot {
    void     (*fn)(void*);
    void*     arg;
    volatile uint32_t done;     // 0 = pending, 1 = complete
    struct ipi_call_slot* next;
} ipi_call_slot_t;

// Push onto `cpu`'s per-CPU call queue.  Lock-free CAS on the head.
// Caller must send VEC_IPI_CALL after pushing and spin on slot->done.
void smp_call_push(uint32_t cpu, ipi_call_slot_t* slot);

// Convenience: push slot + send IPI + wait for done.  Blocks until the
// target runs `fn(arg)` and publishes completion.  Must be called with
// preemption enabled (or at least with IRQs on) so the target can
// actually service the IPI; waiting with IRQs off on the sender would
// still work, but may delay if another CPU is ALSO trying to IPI us.
void smp_call_function_single(uint32_t cpu, void (*fn)(void*), void* arg);
