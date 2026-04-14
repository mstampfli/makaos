#include "preempt.h"
#include "cpu.h"
#include "process.h"
#include "sched.h"
#include "common.h"

// Preemption is a property of the executing CPU, not of the task.  A task
// that is not running cannot be preempted, and a task that IS running can
// only be preempted while sitting on the CPU that runs it.  Tracking the
// counter in cpu_t (not task_t) is both semantically right and strictly
// faster: no dereference through g_current, no cache line shared with
// other CPUs, and the per-CPU access is addressable via GS once APs are
// running.
//
// Interrupts must be disabled around the read-modify-write so a hardware
// IRQ that lands mid-increment and then calls preempt_disable itself
// (e.g. from a driver's IRQ handler) doesn't race with the outer
// increment.  cli/sti is cheap on modern CPUs and doesn't serialize the
// pipeline the way an atomic would.

void preempt_disable(void) {
    __asm__ volatile("cli" ::: "memory");
    this_cpu()->preempt_depth++;
    __asm__ volatile("sti" ::: "memory");
}

void preempt_enable(void) {
    __asm__ volatile("cli" ::: "memory");
    cpu_t* c = this_cpu();
    if (c->preempt_depth == 0) {
        __asm__ volatile("sti" ::: "memory");
        return;  // imbalanced call — ignore
    }
    c->preempt_depth--;
    uint32_t depth = c->preempt_depth;
    __asm__ volatile("sti" ::: "memory");
    // If preemption was pending (timer wanted to switch), do it now.
    if (depth == 0)
        sched_preempt();
}

int preempt_disabled(void) {
    return this_cpu()->preempt_depth > 0;
}
