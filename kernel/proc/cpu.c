#include "cpu.h"
#include "common.h"
#include "acpi.h"
#include "lapic.h"
#include "tss.h"
#include "idt.h"
#include "smp_boot.h"
#include "sched.h"
#include "process.h"
#include "syscall.h"

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

// ── cpu_init_ap ──────────────────────────────────────────────────────────
// The trampoline jumps here in 64-bit long mode with:
//   - rdi = logical cpu_id (index into g_cpus[])
//   - rsp = 16 KiB AP-private kernel stack (from kstack_alloc on the BSP)
//   - IRQs off, CR3 = kernel PML4, SMEP/WP/NXE already on
//
// We must NOT touch this_cpu()/cpu_id()/preempt_disable until GS_BASE is
// programmed — everything below is careful to use the `id` parameter or
// g_cpus[id] directly until the MSR write completes.
//
// Order is the mirror image of the BSP's kmain sequence:
//
//   1. tss_init_ap(id)            — lgdt g_gdt + segment reload (clobbers GS)
//                                    + allocate IST stacks + LTR this CPU's TSS
//   2. program GS_BASE             — this_cpu() is usable from now on
//   3. idt_load_ap()               — lidt the shared kernel vector table
//   4. lapic_init_ap()             — enable x2APIC on this CPU, reset LVT/SVR
//   5. sched_init_idle_for_cpu(id) — kmalloc+init this CPU's idle sentinel
//                                    task and hang it off cpu_t.current
//   6. lapic_timer_start(SCHED_HZ) — arm this CPU's own periodic LAPIC
//                                    timer so sched_tick fires locally
//   7. release bump to g_num_cpus (ATOMIC RELEASE) — the BSP's
//      bring_up_one_ap spin exits after observing this, so every store
//      we make above must be globally visible first.
//   8. sched_yield() into the scheduler.  The AP's rq is empty so
//      do_switch() drops into its sti;hlt idle loop, waking on every
//      timer tick and every VEC_IPI_RESCHEDULE.  sched_tick + context
//      switches advance rcu_qs_count through the normal path, so RCU
//      grace periods complete without the Phase 9-4 pause-spinner.
//
// Never returns.
#define SCHED_HZ 1000
__attribute__((noreturn))
void cpu_init_ap(uint32_t id) {
    // Per-CPU feature MSRs + CR0/CR4 bits that the BSP sets in kmain
    // early-init — none of these live in CR3 / GDT / IDT, so APs inherit
    // NONE of them from the trampoline's mode switch and we must repeat
    // the work here byte-for-byte.
    //
    // IA32_PAT (0x277): keep entry 1 as write-combining so user-mapped
    // framebuffer pages benefit from WC aggregation on this CPU too.
    // Mismatched PAT across CPUs would silently corrupt rendering.
    {
        uint32_t pat_lo = 0x00010406;
        uint32_t pat_hi = 0x00070406;
        __asm__ volatile("wrmsr" : : "a"(pat_lo), "d"(pat_hi), "c"(0x277U));
    }

    // CR0/CR4 — FPU/SSE enable.  Observed bug when this was missing:
    // userland bash tripped #UD (→ SIGILL) on its first movaps/xorps
    // because CR4.OSFXSR wasn't set, SSE was disabled on the AP, and
    // the CPU treated every SSE opcode as invalid.  context_switch's
    // fxrstor also silently no-ops without OSFXSR, so the incoming
    // task's FPU state is whatever the AP happened to boot with.
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uint64_t)(1u << 2));   // clear EM (x87 emulation off)
        cr0 &= ~((uint64_t)(1u << 3));   // clear TS (no task-switch trap)
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);              // OSFXSR:    enable FXSAVE/FXRSTOR + SSE
        cr4 |= (1ULL << 10);             // OSXMMEXCPT: enable unmasked-SIMD-FP #XM
        // SMEP — mirror the BSP's tss_init() setup so user pages can't be
        // executed in kernel mode on any CPU.  Probe CPUID leaf 7 EBX[7]
        // before writing; writing unsupported CR4 bits #GPs.
        {
            uint32_t ebx = 0;
            __asm__ volatile(
                "mov $7, %%eax\n\t"
                "xor %%ecx, %%ecx\n\t"
                "cpuid\n\t"
                : "=b"(ebx) : : "eax", "ecx", "edx"
            );
            if (ebx & (1u << 7)) cr4 |= (1ULL << 20);
        }
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    tss_init_ap(id);
    wrmsr_u64(MSR_IA32_GS_BASE, (uint64_t)&g_cpus[id]);
    idt_load_ap();
    lapic_init_ap();

    // Per-CPU syscall MSRs: IA32_EFER.SCE, STAR, LSTAR, SFMASK.  These
    // are the registers the `syscall` instruction consults to decide
    // the target CS/SS, entry RIP, and RFLAGS mask.  All four are
    // per-CPU MSRs — unset on an AP they'd leave `syscall` as an
    // invalid opcode, which is exactly the #UD → SIGILL path bash
    // hit on AP 1/2 before this call was added.
    syscall_init();

    // Per-CPU idle task — must exist before do_switch runs, because
    // every path through do_switch references this_cpu()->idle to
    // decide whether to re-enqueue the outgoing task.
    sched_init_idle_for_cpu(id);

    // Arm this CPU's LAPIC timer.  Vector VEC_LAPIC_TIMER is already
    // in the shared IDT (BSP registered it in timer_init), so the
    // first tick will land in irq0_entry → timer_irq_handler →
    // sched_tick on THIS CPU, advancing its own rq quantum + rcu_qs.
    lapic_timer_start(SCHED_HZ);

    __atomic_fetch_add(&g_num_cpus, 1u, __ATOMIC_RELEASE);

    serial_puts_dbg("[cpu_ap] online id=");
    serial_hex_dbg((uint64_t)id);

    // Hand off to the scheduler's idle loop.  We cannot use sched_yield
    // here: this CPU's current == idle, so do_switch would do an
    // identity context-switch-to-self and return straight back here —
    // the AP would never actually run the idle body.  sched_enter_idle
    // calls cpu_idle_loop directly so the AP starts executing
    // sti/hlt/sched_yield forever on its own kstack.
    sched_enter_idle();
}
