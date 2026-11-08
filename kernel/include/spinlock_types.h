#pragma once
#include "common.h"

// ── Spinlock TYPES (leaf header) ─────────────────────────────────────────
//
// Just the lock struct definitions, extracted from smp.h so that cpu.h can
// embed a spinlock_t in cpu_t WITHOUT pulling in smp.h.  This matters because
// spin_lock() must disable preemption (a spinlock section is non-preemptible
// by construction), which means smp.h has to include preempt.h -> cpu.h.  If
// cpu.h also included smp.h we would have a cycle; giving cpu.h its lock types
// from this leaf header instead breaks it.
//
// The lock OPERATIONS (spin_lock/spin_unlock/ticket_lock/...) stay in smp.h.
// Include this header only when you need the type but not the operations
// (essentially: cpu.h).  Everyone else includes smp.h as before.

typedef struct {
    volatile uint32_t locked;  // 0 = free, 1 = held
} spinlock_t;

#define SPINLOCK_INIT { 0 }

typedef struct {
    volatile uint32_t head;   // next ticket to issue
    volatile uint32_t tail;   // currently serving
} ticket_lock_t;

#define TICKET_LOCK_INIT { 0, 0 }
