#pragma once
#include "common.h"

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
// Per-task depth counter lives in task_t::preempt_depth.
// Before tasks exist (early boot), g_current == &s_idle which has depth 0;
// preempt_disable on the boot stack works correctly since sched_preempt()
// checks the depth before switching.

void preempt_disable(void);
void preempt_enable(void);

// Returns non-zero if preemption is currently disabled for the running task.
int  preempt_disabled(void);
