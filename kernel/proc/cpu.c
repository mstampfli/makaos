#include "cpu.h"
#include "common.h"
#include "acpi.h"
#include "lapic.h"

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

// Initialise one cpu_t slot.  Used by both cpu_init_bsp and the
// future cpu_init_ap path (Phase 9-4) so every CPU gets identical
// per-CPU state setup.  Caller supplies the logical id (index into
// g_cpus[]) and the hardware APIC id discovered via ACPI or via
// reading LAPIC_ID on the current CPU.
//
// Does NOT program GS_BASE — the caller is responsible for that,
// because BSP and AP setup sequences write GS_BASE at different
// points relative to other CPU initialisation.
static void cpu_slot_init(uint32_t id, uint32_t apic_id) {
    cpu_t* c = &g_cpus[id];

    // Self-pointer FIRST — it's the one field that any later this_cpu()
    // access depends on (via the GS_BASE we program below / in the
    // caller).  Writing it after the spinlock_init would leave a
    // window where gs:0 is NULL.
    c->self               = c;
    c->id                 = id;
    c->apic_id            = apic_id;
    c->current            = NULL;
    c->idle               = NULL;
    // rq and sleep/zombie heads: BSS-zeroed; already NULL.
    spin_lock_init(&c->rq_lock);
    c->preempt_depth      = 0;
    c->reschedule_pending = 0;
    c->sched_ticks        = 0;
    c->context_switches   = 0;
    // slab, pcp, irq_pending: zeroed by BSS.
}

void cpu_init_bsp(void) {
    // Discover BSP's own LAPIC ID now that lapic_init has run.
    uint32_t bsp_apic_id = (uint32_t)lapic_id();

    serial_puts_dbg("[cpu] BSP APIC ID=");
    serial_hex_dbg((uint64_t)bsp_apic_id);
    serial_puts_dbg("[cpu] detected CPUs=");
    serial_hex_dbg((uint64_t)g_acpi.cpu_count);

    // Walk the ACPI CPU list to populate g_cpus[] slots.  The BSP is
    // whichever entry has its apic_id matching the current LAPIC ID.
    // Slot 0 is always the BSP; other slots map 1:1 to the other ACPI
    // entries in discovery order, preserving their APIC IDs.
    //
    // If ACPI parse failed (g_acpi.ok == 0), fall back to a uniprocessor
    // configuration using the LAPIC ID we just read.
    uint32_t n = g_acpi.ok ? g_acpi.cpu_count : 0;
    if (n == 0) {
        // Fallback: just the BSP.
        cpu_slot_init(0, bsp_apic_id);
        g_num_cpus = 1;
    } else {
        // Put the BSP at slot 0 (swap its ACPI entry to the front if
        // firmware didn't list it first).
        uint32_t bsp_idx = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (g_acpi.cpus[i].apic_id == bsp_apic_id) { bsp_idx = i; break; }
        }
        if (bsp_idx != 0) {
            acpi_cpu_t tmp = g_acpi.cpus[0];
            g_acpi.cpus[0] = g_acpi.cpus[bsp_idx];
            g_acpi.cpus[bsp_idx] = tmp;
        }

        // Initialise every discovered CPU slot so GS_BASE on an AP
        // (programmed later during AP bring-up in Phase 9-4) lands
        // in a fully-formed cpu_t.  Slots beyond cpu_count stay BSS-
        // zeroed and are unused.
        for (uint32_t i = 0; i < n && i < MAX_CPUS; i++)
            cpu_slot_init(i, g_acpi.cpus[i].apic_id);

        // g_num_cpus tracks ONLINE CPUs, not hardware-present CPUs.
        // Until Phase 9-4 actually wakes an AP via INIT/SIPI, only
        // the BSP is running; iterators like synchronize_rcu() must
        // not walk slots whose CPUs are still halted, otherwise they
        // wait forever for an rcu_qs_count that never advances.
        // Phase 9-4's cpu_init_ap() increments g_num_cpus atomically
        // as each AP comes online.
        g_num_cpus = 1;
    }

    // Program GS_BASE for the BSP.  Every subsequent this_cpu() call
    // reads `mov %gs:0, %reg` which returns &g_cpus[0].  APs get
    // their own GS_BASE set in the per-AP startup path.
    wrmsr_u64(MSR_IA32_GS_BASE, (uint64_t)&g_cpus[0]);
}

// smp.h declares cpu_id() and num_cpus() but can't define them inline
// because they depend on this_cpu() which needs the full cpu_t definition.
unsigned cpu_id(void)   { return this_cpu_read_u32(id); }
unsigned num_cpus(void) { return g_num_cpus; }
