#pragma once
#include "common.h"
#include "cpu.h"

// ── Preemption control ────────────────────────────────────────────────────
//
// preempt_disable() / preempt_enable() suppress voluntary context switches
// (timer-driven preemption) without masking hardware interrupts.
//
// Unlike cli/sti:
//   - Hardware IRQs still fire and are serviced normally.
//   - The timer ISR still runs and updates tick counts.
//   - sched_preempt() simply defers the context switch until the preemption
//     depth drops back to zero.
//
// Rules:
//   - Calls are nestable: disable/enable pairs must be balanced.
//   - Do NOT sleep (sched_sleep/irq_wait) while preemption is disabled —
//     that would deadlock.  The kernel will panic if this is attempted.
//   - Suitable for short critical sections that touch shared state and must
//     not be interrupted by the scheduler (e.g. driver init sequences).
//
// ── Implementation ───────────────────────────────────────────────────────
//
// The counter lives in cpu_t::preempt_depth (per-CPU, not per-task —
// preemption is a property of the executing context, not the task).
// Disable/enable are single inline instructions:
//
//   preempt_disable:  incl [this_cpu->preempt_depth]
//   preempt_enable:   decl [this_cpu->preempt_depth]
//                     + branch to sched_preempt if depth hit zero
//
// Why single `incl`/`decl` with no LOCK prefix and no cli/sti is safe:
//
//   1. x86 never takes an interrupt mid-instruction — only at instruction
//      boundaries.  So `incl [mem]` is atomic against IRQs landing on the
//      same CPU: the IRQ sees either the pre-value or the post-value,
//      never a torn state.
//
//   2. The counter is strictly per-CPU.  No other CPU will ever read or
//      write this memory location, so we don't need a LOCK prefix for
//      cross-CPU atomicity.  That saves ~20 cycles vs `lock incl`.
//
//   3. If the IRQ handler itself calls preempt_disable/enable, nesting
//      works out: outer sees depth N, IRQ does N→N+1→N, outer continues
//      to N→N+1.
//
// These functions are marked ALWAYS_INLINE so they compile to literal
// single instructions at every call site, regardless of -O level.  Early
// boot is safe because g_cpus[] is BSS-zeroed.

// sched_preempt lives in sched.c; declared here so preempt_enable can
// call it from the inline body.
void sched_preempt(void);

ALWAYS_INLINE void preempt_disable(void) {
    // `incl [mem]`: single-instruction atomic w.r.t. IRQs on this CPU.
    __asm__ volatile("incl %0"
                     : "+m"(this_cpu()->preempt_depth)
                     :
                     : "memory");
}

ALWAYS_INLINE void preempt_enable(void) {
    volatile uint32_t* p = &this_cpu()->preempt_depth;

    // Defensive: ignore imbalanced calls.  Outside the inline asm so it
    // doesn't bloat the fast path — one branch that predicts not-taken.
    if (UNLIKELY(*p == 0)) return;

    // `decl [mem]` sets ZF=1 iff the post-decrement value is zero.
    // Capture ZF via setnz to decide whether to invoke sched_preempt.
    uint8_t nonzero;
    __asm__ volatile("decl %0\n\tsetnz %1"
                     : "+m"(*p), "=r"(nonzero)
                     :
                     : "memory", "cc");

    // Depth just hit zero — if the scheduler wanted to preempt us while
    // we were in the critical section, do the context switch now.
    if (!nonzero) sched_preempt();
}

// Returns non-zero if preemption is currently disabled on this CPU.
ALWAYS_INLINE int preempt_disabled(void) {
    return this_cpu()->preempt_depth > 0;
}
