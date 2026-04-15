#pragma once
#include "common.h"

// ── AP bring-up ──────────────────────────────────────────────────────────
//
// Phase 9-4b/c: walk the ACPI CPU list and bring every non-BSP CPU from
// halt to the scheduler.  Bringing up N APs is O(N) wall clock (each AP
// takes a few hundred microseconds to finish its trampoline); bringup
// itself is serialised because all APs share one trampoline page.
//
// Must be called exactly once, from init_kthread, after:
//   - ACPI parse           (g_acpi populated with all present CPUs)
//   - lapic_init           (x2APIC enabled on the BSP)
//   - vmm_init + pmm_init  (buddy + HHDM live)
//   - tss_init             (shared GDT has a TSS desc per possible CPU)
//   - cpu_init_bsp         (BSP's cpu_t initialised, GS_BASE programmed)
//   - tsc_calibrate        (tsc_read_ns works — we use it for INIT/SIPI delays)
//
// On return, g_num_cpus equals the number of CPUs that successfully came
// online (always >= 1, since the BSP counts).  APs that fail to respond
// within the per-CPU timeout are left halted and logged; the kernel keeps
// running on whatever came up.
void smp_boot_aps(void);

// ── AP C entry point ────────────────────────────────────────────────────
//
// Called from the trampoline after the mode switches.  At entry:
//   - long mode, kernel CR3 loaded
//   - rdi = cpu_id (index into g_cpus[])
//   - rsp = dedicated kernel stack for this AP (16 KiB, guard-paged)
//   - GDT = kernel g_gdt (trampoline loaded its own temp GDT but the
//     absolute far-jmp landed us on kernel code; we reload g_gdt here
//     to drop the trampoline GDT and pick up the per-CPU TSS descriptors)
//
// This function never returns — the AP drops into the scheduler's idle
// loop at the end.
void cpu_init_ap(uint32_t cpu_id);
