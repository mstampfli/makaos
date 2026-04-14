#pragma once
#include "common.h"
#include "smp.h"

// ── Per-CPU state ────────────────────────────────────────────────────────
//
// Every CPU in the system owns a `cpu_t` holding all of its private state:
// the task currently running, the preemption depth, the scheduler run
// queue slot, per-CPU slab magazines, per-CPU page frame cache, IRQ
// bookkeeping.  None of these fields require locking when accessed by the
// CPU that owns them, which is the whole point of the design — fast paths
// never touch a shared cache line.
//
// `this_cpu()` returns the current CPU's cpu_t.  Under Phase 1 (single
// CPU), it always returns &g_cpus[0].  When APs boot in Phase 9, this_cpu()
// grows a real GS-base MSR read.
//
// Ordering note: any code that touches this_cpu()->rq / current / pcp must
// have preemption disabled (or be running inside sched_tick with IRQs
// disabled), otherwise it can be migrated to another CPU mid-operation
// and see a different cpu_t.

struct task_t;

// Forward-declare run_queue_t so it can appear as a field without pulling
// in the scheduler header.  The full definition lives in sched.c and is
// only accessed there.
typedef struct run_queue_t run_queue_t;

// Per-CPU slab magazine — a freelist of recently-freed objects for each
// kmalloc size class.  Placeholder for Phase 4; declared now so cpu_t has
// a stable layout.
#define SLAB_MAGAZINE_CLASSES 10
typedef struct {
    void*    free_head[SLAB_MAGAZINE_CLASSES];
    uint32_t free_count[SLAB_MAGAZINE_CLASSES];
} slab_magazine_t;

// Per-CPU pageset — freelist of recently-freed order-0 pages.  Placeholder
// for Phase 4.
#define PCP_BATCH_SIZE 32
typedef struct {
    uint64_t pages[PCP_BATCH_SIZE];  // physical addresses
    uint32_t count;
} pmm_pcp_t;

typedef struct cpu_t {
    // Identity.
    uint32_t        id;                 // 0..MAX_CPUS-1
    uint32_t        apic_id;            // LAPIC ID (may differ from id)

    // Scheduling.
    struct task_t*  current;            // task currently executing
    struct task_t*  idle;               // this CPU's idle task
    run_queue_t*    rq;                 // per-CPU run queue slot (owned)

    // Preemption: depth counter + pending-reschedule flag.  depth > 0 means
    // voluntary context switches are suppressed; IRQs still fire normally.
    uint32_t        preempt_depth;
    uint32_t        reschedule_pending; // set by sched_wake / timer when a
                                        // context switch should happen as
                                        // soon as preempt_depth hits zero

    // Per-CPU allocator magazines (Phase 4).
    slab_magazine_t slab;
    pmm_pcp_t       pcp;

    // IRQ bookkeeping.  Pending counts for IRQ lines this CPU services.
    // The IRQ waiter lists themselves are per-IRQ (in irq_wait.c) and the
    // IRQ handler always runs on the CPU that's been assigned that line
    // by the IOAPIC, so access is naturally single-CPU in the common path.
    uint8_t         irq_pending[256];

    // RCU quiescent-state counter.  Bumped on every context switch and
    // every idle-loop iteration.  synchronize_rcu() waits until every
    // CPU's counter has advanced since the grace period began.
    // Written non-atomically by the owning CPU; read by any CPU via
    // atomic_load_relaxed (torn reads are fine — they just delay grace
    // period detection by one cycle).
    volatile uint64_t rcu_qs_count;

    // Statistics — non-atomic, owner-only.
    uint64_t        sched_ticks;
    uint64_t        context_switches;
} cpu_t;

// Array of per-CPU slots, sized to MAX_CPUS.  Only slots with a running
// CPU are live; g_num_cpus tracks the count.
extern cpu_t    g_cpus[MAX_CPUS];
extern unsigned g_num_cpus;

// Accessor for the current CPU.  Phase 1: always returns &g_cpus[0].
// Phase 9: reads GS base via an MSR or rdgsbase.
ALWAYS_INLINE cpu_t* this_cpu(void) {
    // TODO SMP: `return (cpu_t*)__rdgsbase();` once APs bring up GS base.
    return &g_cpus[0];
}

// Initialise cpu0's slot.  Called once during kmain, before any task is
// created.  Phase 9 adds per-AP init.
void cpu_init_bsp(void);

// ── Convenience accessors used everywhere ───────────────────────────────
// These exist so call sites don't need to know the cpu_t layout.

ALWAYS_INLINE struct task_t* current_task(void) {
    return this_cpu()->current;
}

ALWAYS_INLINE void set_current_task(struct task_t* t) {
    this_cpu()->current = t;
}
