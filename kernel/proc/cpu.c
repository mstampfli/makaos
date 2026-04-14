#include "cpu.h"
#include "common.h"

// ── Per-CPU slot array ───────────────────────────────────────────────────
// MAX_CPUS slots in BSS.  Slot 0 is the bootstrap processor and is
// initialised by cpu_init_bsp() during kmain.  Slots 1..num_cpus-1 are
// filled in as APs come online (Phase 9).
//
// The struct is zero-initialised by BSS clear, so fields like
// preempt_depth, reschedule_pending, slab magazines, etc. all start at 0
// without needing an explicit init loop.

cpu_t    g_cpus[MAX_CPUS];
unsigned g_num_cpus = 1;    // BSP only until SMP bring-up

void cpu_init_bsp(void) {
    cpu_t* c = &g_cpus[0];
    c->id                 = 0;
    c->apic_id            = 0;   // TODO SMP: read LAPIC ID
    c->current            = NULL;
    c->idle               = NULL;
    c->rq                 = NULL;
    c->preempt_depth      = 0;
    c->reschedule_pending = 0;
    c->sched_ticks        = 0;
    c->context_switches   = 0;
    // slab, pcp, irq_pending: zeroed by BSS.
}

// smp.h declares cpu_id() and num_cpus() but can't define them inline
// because they depend on this_cpu() which needs the full cpu_t definition.
unsigned cpu_id(void)   { return this_cpu()->id; }
unsigned num_cpus(void) { return g_num_cpus; }
