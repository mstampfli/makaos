#pragma once
#include "common.h"
#include "smp.h"     // MAX_CPUS

// ── Task State Segment (64-bit) ───────────────────────────────────────────
// In 64-bit mode the TSS is used for:
//   - RSP0: ring-3 → ring-0 stack pointer (updated on every context switch)
//   - IST1..IST7: dedicated stacks for critical exception vectors
//   - IOPB: we set iopb_offset = sizeof(tss_t) to indicate no I/O permission map
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3];       // RSP0 (ring 0), RSP1, RSP2  — index = ring number
    uint64_t reserved1;
    uint64_t ist[7];       // IST1..IST7  (ist[0] = IST1, ist[1] = IST2, …)
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;  // offset to IOPB from TSS base; = sizeof(tss_t) → no map
} tss_t;

// ── Kernel stack region ───────────────────────────────────────────────────
// Virtual base for ALL kernel stack allocations.
// Lives at PML4[511] → PDPT[511] → PD[16+], safely above the 32MB kernel
// image window (PD[0..15]) and inside pd_kern which every process PML4
// shallow-copies from the kernel PML4.  New mappings added here are
// automatically visible to all processes.
#define KSTACK_PAGES        2                        // 2 × 4KiB = 8KiB per stack
#define KSTACK_SIZE         ((uint64_t)KSTACK_PAGES * PAGE_SIZE)
#define KSTACK_GUARD_SIZE   PAGE_SIZE                // unmapped guard page before each stack
#define KSTACK_SLOT_SIZE    (KSTACK_GUARD_SIZE + KSTACK_SIZE) // 12KiB per slot

// ── GDT layout with one TSS descriptor per CPU ────────────────────────────
// A 64-bit TSS descriptor occupies two 8-byte GDT slots.  To support SMP we
// reserve a pair per possible CPU starting at slot 6, so CPU N's TSS lives
// at GDT[6 + 2*N] / GDT[6 + 2*N + 1]:
//
//   0x00  null
//   0x08  kernel code  CS=0x08  (KERNEL_CS)
//   0x10  kernel data  SS=0x10  (KERNEL_SS)
//   0x18  user data32  placeholder required by sysretq
//   0x20  user data64  SS=0x20
//   0x28  user code64  CS=0x28
//   0x30  TSS[0] low   ← CPU 0    (selector 0x30)
//   0x38  TSS[0] high
//   0x40  TSS[1] low   ← CPU 1    (selector 0x40)
//   0x48  TSS[1] high
//   …
//   At MAX_CPUS CPUs the GDT has (6 + 2*MAX_CPUS) 8-byte slots.
//
// CPU 0 still gets selector 0x30 so single-CPU flows are bit-identical to
// the pre-SMP kernel.  Adding a CPU is free: the BSP pre-populates every
// slot up to MAX_CPUS at boot, pointing at &g_cpus[i].tss, and each AP
// just executes `ltr GDT_TSS_SELECTOR(cpu_id)` during its init.
#define GDT_FIXED_ENTRIES   6
#define GDT_ENTRIES         (GDT_FIXED_ENTRIES + 2 * MAX_CPUS)
#define GDT_TSS_SELECTOR(cpu_id) \
    ((uint16_t)(((GDT_FIXED_ENTRIES) + 2u * (unsigned)(cpu_id)) << 3))

// ── API ───────────────────────────────────────────────────────────────────

// Rebuild the GDT (in kernel BSS) with one TSS descriptor per possible CPU,
// load it, reload all segment registers, set up the BSP's TSS with IST
// stacks, and execute `ltr` for CPU 0.  Call once on the BSP, after
// vmm_init() and kheap is ready (uses pmm_buddy_alloc).
void tss_init(void);

// AP-side TSS setup.  Called from cpu_init_ap() early, BEFORE GS_BASE is
// programmed on the AP (this function clobbers GS_BASE via its segment
// reload; the caller re-programs it immediately after).  Loads the
// shared kernel GDT onto the AP, reloads segments, allocates IST stacks
// into g_cpus[id].tss, and LTRs the CPU's own TSS selector.
// Does NOT touch CR4/SMEP — BSP already did those.
void tss_init_ap(uint32_t id);

// Update TSS.RSP0 — call on every context switch so the CPU knows which
// kernel stack to use when the next ring-3 → ring-0 transition occurs.
// (Also correct to call for kernel threads: it keeps RSP0 sane.)
void tss_set_rsp0(uint64_t rsp0);

// Allocate one kernel stack slot from KSTACK_REGION_VIRT.
// Maps KSTACK_PAGES frames into the kernel PML4 (so all processes see them).
// Returns the virtual address of the TOP of the stack (highest byte + 1),
// ready to use as an initial rsp / RSP0.
// Guard page (immediately below) is intentionally NOT mapped.
virt_addr_t kstack_alloc(void);

// Free a kernel stack previously returned by kstack_alloc.
// `stack_top` must be the value returned by kstack_alloc (top of stack).
void kstack_free(virt_addr_t stack_top);
