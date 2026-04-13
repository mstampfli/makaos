#include "preempt.h"
#include "process.h"
#include "sched.h"
#include "common.h"

void preempt_disable(void) {
    if (!g_current) return;
    g_current->preempt_depth++;
}

void preempt_enable(void) {
    if (!g_current) return;
    if (g_current->preempt_depth == 0) return;  // imbalanced call — ignore
    g_current->preempt_depth--;
    // If preemption was pending (timer wanted to switch), do it now.
    if (g_current->preempt_depth == 0)
        sched_preempt();
}

int preempt_disabled(void) {
    if (!g_current) return 0;
    return g_current->preempt_depth > 0;
}
