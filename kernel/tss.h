#pragma once
#include "common.h"

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

// ── GDT selector for the TSS ──────────────────────────────────────────────
// GDT layout rebuilt by tss_init() in kernel BSS:
//   0x00  null
//   0x08  null  (placeholder; some ABIs want 32-bit CS here, we don't need it)
//   0x10  kernel data  (SS=DS=ES=0x10)
//   0x18  kernel code  (CS=0x18  — must match KERNEL_CS in common.h)
//   0x20  TSS low  (64-bit TSS descriptor occupies TWO 8-byte GDT slots)
//   0x28  TSS high
#define GDT_TSS_SELECTOR    0x30

// ── API ───────────────────────────────────────────────────────────────────

// Rebuild the GDT (in kernel BSS) with a TSS descriptor, load it, reload
// all segment registers, set up the TSS with IST stacks, and execute `ltr`.
// Call once, after vmm_init() and kheap is ready (uses pmm_buddy_alloc).
void tss_init(void);

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
