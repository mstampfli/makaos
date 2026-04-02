#include "tss.h"
#include "vmm.h"
#include "pmm.h"

// ── Static TSS ───────────────────────────────────────────────────────────
// One TSS for the whole system (single CPU).  Placed in .bss so it is
// zeroed before tss_init() runs.
tss_t g_tss;

// ── GDT in kernel BSS ─────────────────────────────────────────────────────
// We rebuild the GDT here so we can append a 64-bit TSS descriptor (16 bytes
// = two 8-byte slots).  Keeping CS=0x18 and SS/DS/ES=0x10 means we never
// have to reload segment registers after the lgdt — they still point to valid
// descriptors at the same selectors.
//
// 64-bit TSS descriptor layout (two consecutive 64-bit words):
//   Word 0 (low):
//     [15: 0] limit[15:0]
//     [31:16] base[15:0]
//     [39:32] base[23:16]
//     [43:40] type = 0b1001 (64-bit available TSS)
//     [44]    S    = 0 (system descriptor)
//     [46:45] DPL  = 00 (kernel only)
//     [47]    P    = 1 (present)
//     [51:48] limit[19:16]
//     [52]    AVL  = 0
//     [53]    L    = 0
//     [54]    DB   = 0
//     [55]    G    = 0 (byte granularity — limit is in bytes)
//     [63:56] base[31:24]
//   Word 1 (high):
//     [31: 0] base[63:32]
//     [63:32] reserved (must be 0)
static uint64_t g_gdt[8];  // 8 × 8 bytes = 64 bytes; zeroed by BSS clear

// Slot counter and dynamic base for kstack_alloc
static uint32_t g_kstack_slots = 0;
static virt_addr_t g_kstack_region_base = 0;

// ── Internal: get physical address of kernel PML4 ───────────────────────
// vmm_kernel_pml4_get is exposed in vmm.h.
// kstack_alloc must target the KERNEL PML4 (not whatever CR3 happens to be),
// because stacks must be mapped in the kernel's own address space and then
// inherited by all processes via the shallow-copied upper half.

// ── Internal: write TSS descriptor into g_gdt[6] and g_gdt[7] ──────────
static void gdt_write_tss(void) {
    uint64_t base  = (uint64_t)&g_tss;
    uint32_t limit = (uint32_t)(sizeof(tss_t) - 1);

    // Low word
    g_gdt[6] =
        ((uint64_t)(limit & 0xFFFFU))              |   // limit[15:0]
        ((uint64_t)(base  & 0xFFFFU)       << 16)  |   // base[15:0]
        ((uint64_t)((base >> 16) & 0xFFU)  << 32)  |   // base[23:16]
        ((uint64_t)0x89ULL                 << 40)  |   // P=1, DPL=0, type=0x9 (TSS avail)
        ((uint64_t)((limit >> 16) & 0xFU)  << 48)  |   // limit[19:16]
        ((uint64_t)((base >> 24) & 0xFFU)  << 56);     // base[31:24]

    // High word: upper 32 bits of base, rest 0
    g_gdt[7] = (uint64_t)(base >> 32);
}

// ── tss_init ─────────────────────────────────────────────────────────────
void tss_init(void) {
    // 1. Populate the TSS: set iopb_offset so the CPU finds no IOPB.
    //    IST stacks allocated below after the GDT is live.
    g_tss.iopb_offset = (uint16_t)sizeof(tss_t);

    // 2. Build the new GDT.
    //
    //   0x00  null
    //   0x08  kernel code  CS=0x08  (KERNEL_CS)   L=1, DPL=0
    //   0x10  kernel data  SS=0x10  (KERNEL_SS)   DPL=0
    //   0x18  user data32           placeholder required by sysretq for compat mode
    //   0x20  user data64  SS=0x20  (USER_SS&~3)  DPL=3
    //   0x28  user code64  CS=0x28  (USER_CS&~3)  L=1, DPL=3
    //   0x30  TSS low   (filled by gdt_write_tss)
    //   0x38  TSS high
    //
    // STAR encodes: kernel base = 0x08 (CS=0x08, SS=0x08+8=0x10 ✓)
    //               user   base = 0x18 (sysretq: SS=0x18+8=0x20, CS=0x18+16=0x28 ✓)
    g_gdt[0] = 0x0000000000000000ULL; // null
    g_gdt[1] = 0x00AF9A000000FFFFULL; // kernel code  CS=0x08  L=1
    g_gdt[2] = 0x00CF93000000FFFFULL; // kernel data  SS=0x10
    g_gdt[3] = 0x00CFF3000000FFFFULL; // user data32  0x18  (DPL=3 placeholder)
    g_gdt[4] = 0x00CFF3000000FFFFULL; // user data64  SS=0x20  DPL=3
    g_gdt[5] = 0x00AFFA000000FFFFULL; // user code64  CS=0x28  L=1, DPL=3
    g_gdt[6] = 0x0000000000000000ULL; // TSS low  (filled below)
    g_gdt[7] = 0x0000000000000000ULL; // TSS high

    gdt_write_tss();

    // 3. Load the new GDT.
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    gdtr.base  = (uint64_t)g_gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");

    // 4. Reload CS via a far return.
    //    After lgdt the CPU still caches the old descriptor in the hidden
    //    part of CS.  An lretq with CS=0x08 (new KERNEL_CS) forces a reload.
    __asm__ volatile(
        "pushq $0x08\n\t"           // push new CS (KERNEL_CS)
        "lea 1f(%%rip), %%rax\n\t"  // push return RIP (label 1:)
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : : "rax", "memory"
    );

    // 5. Reload data-segment registers to KERNEL_SS=0x10.
    __asm__ volatile(
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        : : : "ax", "memory"
    );

    // 6. Allocate IST stacks AFTER the GDT/TSS are live so we can use
    //    vmm_page_map.  We allocate BEFORE ltr because ltr only reads the
    //    descriptor; the IST pointers are read at interrupt delivery time.
    //
    //    IST1 → #DF (double fault, vector 8)
    //    IST2 → #NMI (non-maskable interrupt, vector 2)
    g_tss.ist[0] = (uint64_t)kstack_alloc(); // IST1
    g_tss.ist[1] = (uint64_t)kstack_alloc(); // IST2

    // Set RSP0 to something sane (will be overwritten per-process by sched).
    // Use IST1's top as a temporary safe value.
    g_tss.rsp[0] = g_tss.ist[0];

    // 7. Load the TSS register.
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS_SELECTOR) : "memory");

    // 8. Enable SMEP (CR4 bit 20) if the CPU supports it.
    //    SMEP: kernel cannot execute user-space pages.
    //    Check CPUID leaf 7, EBX bit 7 before setting — writing unsupported
    //    CR4 bits causes #GP.
    {
        uint32_t ebx = 0;
        __asm__ volatile(
            "mov $7, %%eax\n\t"
            "xor %%ecx, %%ecx\n\t"
            "cpuid\n\t"
            : "=b"(ebx) : : "eax", "ecx", "edx"
        );
        if (ebx & (1U << 7)) {  // SMEP supported
            uint64_t cr4;
            __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1ULL << 20);
            __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
        }
    }
}

// ── tss_set_rsp0 ─────────────────────────────────────────────────────────
void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp[0] = rsp0;
}

// ── kstack_alloc ─────────────────────────────────────────────────────────
virt_addr_t kstack_alloc(void) {
    if (!g_kstack_region_base) {
        // Round __kernel_end up to the next 2MB boundary so we start at a
        // fresh PD entry, never overlapping the kernel image no matter its size.
        virt_addr_t end = (virt_addr_t)__kernel_end;
        g_kstack_region_base = (end + (2ULL * 1024 * 1024 - 1))
                               & ~(2ULL * 1024 * 1024 - 1);
    }

    uint32_t slot = g_kstack_slots++;

    // Base of this slot's stack pages (guard page immediately below this).
    virt_addr_t stack_base = g_kstack_region_base
                           + (virt_addr_t)slot * KSTACK_SLOT_SIZE
                           + KSTACK_GUARD_SIZE; // skip the guard page

    // Always map into the KERNEL PML4 so the mapping is shared with all
    // processes (they inherit PML4[511] upper half from the kernel PML4).
    phys_addr_t kpml4 = vmm_kernel_pml4_get();

    for (uint32_t i = 0; i < KSTACK_PAGES; i++) {
        phys_addr_t phys = pmm_buddy_alloc(0);
        // vmm_page_map handles intermediate table creation; VMM_KDATA = P|RW|NX
        vmm_page_map(kpml4, stack_base + (virt_addr_t)i * PAGE_SIZE, phys, VMM_KDATA);
    }

    // Return the TOP (highest address + 1 byte).  Stacks grow downward.
    return stack_base + KSTACK_SIZE;
}

// ── kstack_free ──────────────────────────────────────────────────────────
void kstack_free(virt_addr_t stack_top) {
    // stack_top is one byte past the last byte of the stack.
    // The KSTACK_PAGES pages start at stack_top - KSTACK_SIZE.
    virt_addr_t stack_base = stack_top - KSTACK_SIZE;
    phys_addr_t kpml4 = vmm_kernel_pml4_get();

    for (uint32_t i = 0; i < KSTACK_PAGES; i++) {
        phys_addr_t phys;
        if (vmm_page_unmap(kpml4, stack_base + (virt_addr_t)i * PAGE_SIZE, &phys))
            pmm_buddy_free(phys, 0);
    }
}
