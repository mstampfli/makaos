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
// Per-CPU depth counter lives in cpu_t::preempt_depth (see cpu.h).
// Early boot is safe: g_cpus[] is BSS-zeroed, so the depth reads as 0
// before cpu_init_bsp() runs.  sched_preempt() checks the depth before
// switching so preempt_disable on the boot stack is correct from the
// very first instruction.

void preempt_disable(void);
void preempt_enable(void);

// Returns non-zero if preemption is currently disabled for the running task.
int  preempt_disabled(void);
