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

// IA32_GS_BASE — the MSR that backs the GS segment base in long mode.
// Writing it points GS at our per-CPU cpu_t, so `mov %gs:0, %reg` reads
// the cpu_t self-pointer in a single instruction.  IA32_KERNEL_GS_BASE
// (0xC0000102) is the swapgs target; we never use swapgs in this kernel
// (userland never touches GS) so we leave it alone.
#define MSR_IA32_GS_BASE 0xC0000101u

static inline void wrmsr_u64(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void cpu_init_bsp(void) {
    cpu_t* c = &g_cpus[0];

    // Self-pointer FIRST — must be set before GS_BASE is programmed,
    // otherwise the very first this_cpu() call would dereference a
    // stale or zero MSR.
    c->self               = c;

    c->id                 = 0;
    c->apic_id            = 0;   // populated by lapic_init() once LAPIC is up
    c->current            = NULL;
    c->idle               = NULL;
    // rq and sleep/zombie heads: BSS-zeroed; already NULL.
    spin_lock_init(&c->rq_lock);
    c->preempt_depth      = 0;
    c->reschedule_pending = 0;
    c->sched_ticks        = 0;
    c->context_switches   = 0;
    // slab, pcp, irq_pending: zeroed by BSS.

    // Program GS_BASE so this_cpu() works for every subsequent caller.
    // From here on, `mov %gs:0, %reg` returns a valid cpu_t*.
    wrmsr_u64(MSR_IA32_GS_BASE, (uint64_t)c);
}

// smp.h declares cpu_id() and num_cpus() but can't define them inline
// because they depend on this_cpu() which needs the full cpu_t definition.
unsigned cpu_id(void)   { return this_cpu_read_u32(id); }
unsigned num_cpus(void) { return g_num_cpus; }
