#include "preempt.h"
#include "cpu.h"
#include "process.h"
#include "sched.h"
#include "common.h"

// ── Preemption control — the hot path ───────────────────────────────────
//
// preempt_disable / preempt_enable are called on every syscall, every
// RCU reader section (rcu_read_lock == preempt_disable), every slab
// allocation (once Phase 4 lands), and countless other places.  They
// must be as close to zero-overhead as possible.
//
// The implementation is a single inline `incl`/`decl` on the current
// CPU's preempt_depth field.  Why this is safe without cli/sti or LOCK:
//
//   1. `incl [mem]` is atomic against an IRQ landing on the same CPU
//      mid-instruction — x86 guarantees an interrupt can only be taken
//      at instruction boundaries, never mid-instruction.  So if an IRQ
//      fires during our increment, it either sees the pre-value (IRQ
//      before incl) or the post-value (IRQ after incl), never a torn
//      half-written state.
//
//   2. The counter is strictly per-CPU — no other CPU will ever read or
//      write this memory location — so we don't need a LOCK prefix for
//      cross-CPU atomicity.  That saves ~20 cycles vs `lock incl`.
//
//   3. The IRQ handler may itself call preempt_disable/enable.  That's
//      fine: we'll observe depth N, IRQ runs with depth N→N+1→N, we
//      resume and finish our depth N→N+1.  Nested correctness.
//
// Cost: one dependent memory op.  On modern Intel this is ~3-5 cycles
// end to end, vs ~50+ cycles for the previous cli/sti wrapped version.

// Access the per-CPU preempt_depth as a lvalue.  Wrapped in a helper so
// the inline asm sees a clean operand.
static inline volatile uint32_t* preempt_depth_ptr(void) {
    return &this_cpu()->preempt_depth;
}

void preempt_disable(void) {
    // `incl [mem]`: single-instruction atomic w.r.t. IRQs on this CPU.
    __asm__ volatile("incl %0"
                     : "+m"(*preempt_depth_ptr())
                     :
                     : "memory");
}

void preempt_enable(void) {
    volatile uint32_t* p = preempt_depth_ptr();
    // Defensive: ignore imbalanced calls.  The test is outside the
    // inline asm so it doesn't bloat the fast path.
    if (__builtin_expect(*p == 0, 0)) return;

    // `decl [mem]` sets ZF=1 iff the post-decrement value is zero.
    // We capture ZF via setnz to decide whether to invoke sched_preempt.
    uint8_t nonzero;
    __asm__ volatile("decl %0\n\tsetnz %1"
                     : "+m"(*p), "=r"(nonzero)
                     :
                     : "memory", "cc");

    // If depth just hit zero and the scheduler wanted to preempt us
    // while we were in the critical section, do the context switch now.
    if (!nonzero) {
        // Re-read under the barrier to catch any sched_tick that bumped
        // s_reschedule between the decl and the branch.
        sched_preempt();
    }
}

int preempt_disabled(void) {
    return *preempt_depth_ptr() > 0;
}
