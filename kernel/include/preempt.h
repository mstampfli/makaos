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
// single-instruction operations at every call site.  After the cpu.h
// gs-relative refactor:
//
//   preempt_disable()  →  incl %gs:offset(preempt_depth)
//   preempt_enable()   →  decl %gs:offset(preempt_depth)  + setnz
//                        + maybe-call sched_preempt
//
// That's literally one ~1-cycle instruction on the common path.  No
// indirection through this_cpu(), no register pressure, no memory
// load to fetch the cpu_t pointer.

// sched_preempt lives in sched.c; declared here so preempt_enable can
// call it from the inline body.
void sched_preempt(void);

ALWAYS_INLINE void preempt_disable(void) {
    // `incl %gs:offset` — single instruction, single cycle.  Atomic
    // against IRQs on this CPU (x86 never preempts mid-instruction);
    // no LOCK prefix because the memory is per-CPU.
    this_cpu_inc_u32(preempt_depth);
}

ALWAYS_INLINE void preempt_enable(void) {
    // Defensive: ignore imbalanced calls.  Reading the field via a
    // gs-relative load is one instruction; the branch predicts
    // not-taken in the common case (counter > 0 about to drop to 0).
    if (UNLIKELY(this_cpu_read_u32(preempt_depth) == 0)) return;

    // `decl %gs:offset; setnz %al` — atomic post-decrement on this
    // CPU's counter, captures whether the result is non-zero in one
    // instruction pair.  No LOCK prefix.
    uint8_t nonzero = this_cpu_dec_u32_nonzero(preempt_depth);

    // Depth just hit zero — if the scheduler wanted to preempt us
    // while we were in the critical section, do the context switch
    // now.
    if (!nonzero) sched_preempt();
}

// Leave a preempt-disabled section WITHOUT the resched check preempt_enable
// does -- for IRQ / timer-tick / atomic-completion contexts where a context
// switch is forbidden.  Single source of truth for the bare per-CPU decrement
// that ~10 such paths used to open-code as `this_cpu()->preempt_depth--`.
// Caller guarantees balance (paired with a preceding preempt_disable), so no
// zero-guard -- identical to the hand-rolled decrement it replaces.
ALWAYS_INLINE void preempt_enable_no_resched(void) {
    (void)this_cpu_dec_u32_nonzero(preempt_depth);
}

// Returns non-zero if preemption is currently disabled on this CPU.
ALWAYS_INLINE int preempt_disabled(void) {
    return this_cpu_read_u32(preempt_depth) > 0;
}
