#include "vmm.h"
#include "pmm.h"
#include "idt.h"
#include "mm.h"
#include "shmem.h"

// Physical address of the kernel's own PML4.
// Set by vmm_init(); used by vmm_alloc_pml4() to clone kernel entries.
static phys_addr_t g_kernel_pml4 = 0;

// ── Index extraction (9 bits per level) ──────────────────────────────────
// A 64-bit canonical virtual address is split as:
//   [63:48] sign extension (must mirror bit 47)
//   [47:39] PML4 index  (9 bits → 0..511)
//   [38:30] PDPT index  (9 bits → 0..511)
//   [29:21] PD   index  (9 bits → 0..511)
//   [20:12] PT   index  (9 bits → 0..511)
//   [11: 0] byte offset within the 4KiB page
#define PML4_IDX(v) (((uint64_t)(v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((uint64_t)(v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((uint64_t)(v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((uint64_t)(v) >> 12) & 0x1FF)

// ── Internal helpers ──────────────────────────────────────────────────────

static inline void invlpg(virt_addr_t vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline phys_addr_t vmm_pml4_get(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PAGE_ADDR_MASK;
}

phys_addr_t vmm_current_pml4(void) { return vmm_pml4_get(); }

static inline void zero_page(virt_addr_t addr) {
    uint64_t* p = (uint64_t*)addr;
    for (int i = 0; i < 512; i++) p[i] = 0;
}

// Walk the 4-level page table for `vaddr` inside the PML4 at `pml4_phys`.
// Returns a pointer to the leaf PTE (inside HHDM so we can read/write it).
// If `create` is non-zero and an intermediate table is missing, allocates
// a fresh zeroed frame for it.
//
// `inter_flags`: flags written into newly created INTERMEDIATE entries.
//   For kernel mappings: PAGE_PRESENT | PAGE_WRITABLE
//   For user   mappings: PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER
//
// IMPORTANT — the US bit in intermediate levels:
//   The CPU enforces the MOST RESTRICTIVE combination across all four levels.
//   If any level in the walk has US=0 (supervisor-only), ring-3 is blocked —
//   even if the leaf PTE has PAGE_USER set.  So for user pages every
//   intermediate entry also needs PAGE_USER.
static pte_t* vmm_pte_get(phys_addr_t pml4_phys, virt_addr_t vaddr,
                           uint8_t create, uint64_t inter_flags) {
    uint8_t  shifts[3] = {39, 30, 21};
    phys_addr_t cur = pml4_phys;

    for (int i = 0; i < 3; i++) {
        uint64_t idx   = (vaddr >> shifts[i]) & 0x1FF;
        uint64_t* tbl  = (uint64_t*)(cur + HHDM_OFFSET);
        uint64_t  entry = tbl[idx];

        if (!(entry & PAGE_PRESENT)) {
            if (!create) return NULL;

            phys_addr_t new_frame = pmm_buddy_alloc(0);
            if (new_frame == PMM_INVALID_ADDR) return NULL;

            zero_page(new_frame + HHDM_OFFSET);
            entry = new_frame | inter_flags | PAGE_PRESENT;
            tbl[idx] = entry;
        }

        cur = entry & PAGE_ADDR_MASK;
    }

    uint64_t* pt = (uint64_t*)(cur + HHDM_OFFSET);
    return &pt[PT_IDX(vaddr)];
}

// ── Public API ────────────────────────────────────────────────────────────

phys_addr_t vmm_kernel_pml4_get(void) {
    return g_kernel_pml4;
}

// ── vmm_map_mmio ──────────────────────────────────────────────────────────
// Maps physical MMIO pages into a private kernel window (0xFFFF900000000000+).
// Pages are mapped uncacheable (PCD+PWT) so device register reads/writes are
// not buffered or reordered by the CPU cache.
#define MMIO_VIRT_BASE 0xFFFF900000000000ULL
static virt_addr_t s_mmio_next = MMIO_VIRT_BASE;

virt_addr_t vmm_map_mmio(phys_addr_t phys, uint64_t bytes) {
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    virt_addr_t base = s_mmio_next;
    uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_PCD | PAGE_PWT;
    for (uint64_t i = 0; i < pages; i++) {
        vmm_page_map(g_kernel_pml4,
                     base + i * PAGE_SIZE,
                     phys + i * PAGE_SIZE,
                     flags);
    }
    s_mmio_next += pages * PAGE_SIZE;
    return base;
}

// ── vmm_map_physical_user ─────────────────────────────────────────────────
// Maps a contiguous physical range into a user address space.
// Used for MMIO-style mappings (e.g. framebuffer).  Pages are mapped
// immediately with write-through (PWT) and user-accessible flags.
// Creates a VMA_MMIO VMA so fork/CoW skip these pages.
virt_addr_t vmm_map_physical_user(mm_t* mm, phys_addr_t pml4_phys,
                                  phys_addr_t phys, uint64_t bytes) {
    if (!mm || !bytes) return 0;

    uint64_t len = (bytes + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
    virt_addr_t vaddr = mm_vma_find_free(mm, len);
    if (!vaddr) return 0;

    // Add a VMA: readable + writable + MMIO (no CoW, no swap, no demand-page).
    if (!mm_vma_add(mm, vaddr, vaddr + len, VMA_R | VMA_W | VMA_MMIO))
        return 0;

    // Map all pages immediately — MMIO is not demand-paged.
    uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_PWT | PAGE_NX;
    uint64_t npages = len / PAGE_SIZE;
    for (uint64_t i = 0; i < npages; i++) {
        vmm_page_map(pml4_phys,
                     vaddr + i * PAGE_SIZE,
                     phys  + i * PAGE_SIZE,
                     flags);
    }
    return vaddr;
}

void vmm_init(phys_addr_t kernel_pml4_phys) {
    g_kernel_pml4 = kernel_pml4_phys;

    // The GDT was set up by the loader at a low physical address accessible
    // only via the identity map (PML4[0]).  Per-process PML4s zero PML4[0],
    // so hardware interrupts would page-fault trying to read GDT descriptors.
    // Fix: reload GDTR with the HHDM virtual address of the GDT so it is
    // reachable from any PML4 via PML4[256] (HHDM, always inherited).
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    gdtr.base += HHDM_OFFSET;
    __asm__ volatile("lgdt %0" : : "m"(gdtr));
}

// Allocate a new PML4 for a process.
//
// Lower half (PML4[0..255]): zeroed.
//   Each process starts with a completely empty user address space.
//   You'll map pages into it with vmm_page_map before switching to it.
//
// Upper half (PML4[256..511]): shallow-copied from the kernel PML4.
//   This means every process automatically shares the kernel's higher-half
//   and HHDM mappings.  Kernel code running in a process's context still
//   works because CR3 contains kernel entries.  This is the same approach
//   used by Linux (with Meltdown mitigations aside).
phys_addr_t vmm_alloc_pml4(void) {
    phys_addr_t phys = pmm_buddy_alloc(0);
    if (phys == PMM_INVALID_ADDR) return PMM_INVALID_ADDR;

    uint64_t* new_pml4  = (uint64_t*)(phys + HHDM_OFFSET);
    uint64_t* kern_pml4 = (uint64_t*)(g_kernel_pml4 + HHDM_OFFSET);

    for (int i = 0;   i < 256; i++) new_pml4[i] = 0;             // user: empty
    for (int i = 256; i < 512; i++) new_pml4[i] = kern_pml4[i]; // kernel: shared

    return phys;
}

// Map one 4KiB page.
// flags: use VMM_UDATA, VMM_UCODE, VMM_KDATA, VMM_KCODE etc.
// If PAGE_USER is in flags, intermediate entries also get PAGE_USER (required
// for ring-3 traversal to reach the leaf entry).
uint8_t vmm_page_map(phys_addr_t pml4_phys, virt_addr_t vaddr,
                     phys_addr_t paddr, uint64_t flags) {
    // Derive intermediate flags: always P|RW, plus USER if this is a user page.
    uint64_t inter = PAGE_PRESENT | PAGE_WRITABLE;
    if (flags & PAGE_USER) inter |= PAGE_USER;

    pte_t* pte = vmm_pte_get(pml4_phys, vaddr, 1, inter);
    if (!pte) return 0;

    *pte = (paddr & PAGE_ADDR_MASK) | flags;
    invlpg(vaddr);
    return 1;
}

// Unmap one 4KiB page.  Optionally returns the old physical address.
// Does NOT free the physical frame.
uint8_t vmm_page_unmap(phys_addr_t pml4_phys, virt_addr_t vaddr,
                       phys_addr_t* out_paddr) {
    pte_t* pte = vmm_pte_get(pml4_phys, vaddr, 0, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;

    if (out_paddr) *out_paddr = *pte & PAGE_ADDR_MASK;
    *pte = 0;
    invlpg(vaddr);
    return 1;
}

phys_addr_t vmm_page_phys(phys_addr_t pml4_phys, virt_addr_t vaddr) {
    pte_t* pte = vmm_pte_get(pml4_phys, vaddr, 0, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return PMM_INVALID_ADDR;
    return *pte & PAGE_ADDR_MASK;
}

// ── vmm_get_user_pages ───────────────────────────────────────────────────
// Resolve `count` consecutive pages starting at page-aligned `uaddr` into
// HHDM kernel pointers.  If a page is not yet present (demand-page), we
// allocate a frame and map it writable+user+NX — same as the page-fault
// handler would do.  No CoW concern: MakaOS fork does full deep-copy.
//
// Returns `count` on success, 0 on any failure (OOM, bad address).
uint32_t vmm_get_user_pages(phys_addr_t pml4_phys, virt_addr_t uaddr,
                            uint32_t count, void** out) {
    uint64_t inter = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t leaf  = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX;

    for (uint32_t i = 0; i < count; i++) {
        virt_addr_t va = (uaddr & ~0xFFFULL) + (uint64_t)i * PAGE_SIZE;

        // Try to find existing mapping.
        pte_t* pte = vmm_pte_get(pml4_phys, va, 0, 0);

        if (pte && (*pte & PAGE_PRESENT)) {
            // Page exists — use its physical frame.
            phys_addr_t phys = *pte & PAGE_ADDR_MASK;
            out[i] = (void*)(phys + HHDM_OFFSET);
        } else {
            // Not mapped yet — demand-allocate (mirrors page-fault handler).
            phys_addr_t frame = pmm_buddy_alloc(0);
            if (frame == PMM_INVALID_ADDR) return 0;
            // Zero the frame (same as page-fault handler).
            uint8_t* p = (uint8_t*)(frame + HHDM_OFFSET);
            for (int b = 0; b < (int)PAGE_SIZE; b++) p[b] = 0;
            // Create intermediate tables if needed and map.
            pte = vmm_pte_get(pml4_phys, va, 1, inter);
            if (!pte) { pmm_buddy_free(frame, 0); return 0; }
            *pte = frame | leaf;
            out[i] = (void*)(frame + HHDM_OFFSET);
        }
    }
    return count;
}

// Allocate a physical frame, map it at vaddr in the CURRENT address space.
void* vmm_page_alloc(virt_addr_t vaddr, uint64_t flags) {
    phys_addr_t frame = pmm_buddy_alloc(0);
    if (frame == PMM_INVALID_ADDR) return NULL;

    if (!vmm_page_map(vmm_pml4_get(), vaddr, frame, flags)) {
        pmm_buddy_free(frame, 0);
        return NULL;
    }
    return (void*)vaddr;
}

// Unmap and free the frame at vaddr in the CURRENT address space.
void vmm_page_free(virt_addr_t vaddr) {
    phys_addr_t frame;
    if (vmm_page_unmap(vmm_pml4_get(), vaddr, &frame))
        pmm_buddy_free(frame, 0);
}

// Switch to a different address space.
// Writing CR3 flushes the entire TLB (except global pages, which we don't use).
void vmm_switch(phys_addr_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

// Free all LOWER-HALF (user-space) page TABLE frames in a PML4.
// Walks PML4[0..255] → frees every PDPT, PD, PT frame it finds.
// Does NOT free the physical pages the process had mapped (its data/code/stack).
// Does NOT free the PML4 frame itself — caller does that.
// Free all user-space page table structures and leaf frames.
// For shared VMAs (backed by shmem_t), leaf frames are NOT freed here —
// they are owned by the shmem object and freed when its refcount hits 0.
// If mm is NULL, ALL leaf frames are freed (legacy behavior for error paths
// where no shmem could have been attached).
void vmm_free_user_ex(phys_addr_t pml4_phys, mm_t* mm) {
    uint64_t* pml4 = (uint64_t*)(pml4_phys + HHDM_OFFSET);

    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)((pml4[i] & PAGE_ADDR_MASK) + HHDM_OFFSET);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            uint64_t* pd = (uint64_t*)((pdpt[j] & PAGE_ADDR_MASK) + HHDM_OFFSET);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT)) continue;
                uint64_t* pt = (uint64_t*)((pd[k] & PAGE_ADDR_MASK) + HHDM_OFFSET);

                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PAGE_PRESENT)) continue;
                    phys_addr_t leaf = pt[l] & PAGE_ADDR_MASK;

                    // Reconstruct the virtual address to look up the VMA.
                    virt_addr_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30)
                                   | ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    int skip_free = 0;
                    if (mm) {
                        vma_t* vma = mm_vma_find(mm, va);
                        if (vma && (vma->shmem || (vma->flags & VMA_MMIO)))
                            skip_free = 1;
                    }

                    if (!skip_free)
                        pmm_buddy_free(leaf, 0);
                }
                pmm_buddy_free(pd[k] & PAGE_ADDR_MASK, 0);     // free PT frame
            }
            pmm_buddy_free(pdpt[j] & PAGE_ADDR_MASK, 0);       // free PD frame
        }
        pmm_buddy_free(pml4[i] & PAGE_ADDR_MASK, 0);           // free PDPT frame
    }
    // Caller frees the PML4 frame itself
}

// Legacy wrapper: frees all leaf frames unconditionally.
// Safe for error-cleanup paths where no shmem could exist.
void vmm_free_user(phys_addr_t pml4_phys) {
    vmm_free_user_ex(pml4_phys, NULL);
}

// ── vmm_clone_user ────────────────────────────────────────────────────────
// Clone all user (lower half) page table entries from src to dst PML4.
// For private pages: allocate a new frame and deep-copy 4096 bytes.
// For shared pages (VMA has shmem backing): map the SAME physical frame
// (no copy — both parent and child see the same memory).
// If src_mm is NULL, all pages are treated as private (legacy behavior).
// Returns 1 on success, 0 on OOM.
uint8_t vmm_clone_user_ex(phys_addr_t dst_pml4, phys_addr_t src_pml4,
                          mm_t* src_mm) {
    uint64_t* src_pml4v = (uint64_t*)(src_pml4 + HHDM_OFFSET);
    uint64_t* dst_pml4v = (uint64_t*)(dst_pml4 + HHDM_OFFSET);

    for (int pi = 0; pi < 256; pi++) {
        if (!(src_pml4v[pi] & PAGE_PRESENT)) continue;

        phys_addr_t dst_pdpt_phys = pmm_buddy_alloc(0);
        if (dst_pdpt_phys == PMM_INVALID_ADDR) return 0;
        zero_page(dst_pdpt_phys + HHDM_OFFSET);

        uint64_t inter_flags = (src_pml4v[pi] & ~PAGE_ADDR_MASK) & 0xFFF;
        dst_pml4v[pi] = dst_pdpt_phys | inter_flags | PAGE_PRESENT;

        uint64_t* src_pdpt = (uint64_t*)((src_pml4v[pi] & PAGE_ADDR_MASK) + HHDM_OFFSET);
        uint64_t* dst_pdpt = (uint64_t*)(dst_pdpt_phys + HHDM_OFFSET);

        for (int qi = 0; qi < 512; qi++) {
            if (!(src_pdpt[qi] & PAGE_PRESENT)) continue;

            phys_addr_t dst_pd_phys = pmm_buddy_alloc(0);
            if (dst_pd_phys == PMM_INVALID_ADDR) return 0;
            zero_page(dst_pd_phys + HHDM_OFFSET);

            uint64_t inter_flags2 = (src_pdpt[qi] & ~PAGE_ADDR_MASK) & 0xFFF;
            dst_pdpt[qi] = dst_pd_phys | inter_flags2 | PAGE_PRESENT;

            uint64_t* src_pd = (uint64_t*)((src_pdpt[qi] & PAGE_ADDR_MASK) + HHDM_OFFSET);
            uint64_t* dst_pd = (uint64_t*)(dst_pd_phys + HHDM_OFFSET);

            for (int ri = 0; ri < 512; ri++) {
                if (!(src_pd[ri] & PAGE_PRESENT)) continue;

                phys_addr_t dst_pt_phys = pmm_buddy_alloc(0);
                if (dst_pt_phys == PMM_INVALID_ADDR) return 0;
                zero_page(dst_pt_phys + HHDM_OFFSET);

                uint64_t inter_flags3 = (src_pd[ri] & ~PAGE_ADDR_MASK) & 0xFFF;
                dst_pd[ri] = dst_pt_phys | inter_flags3 | PAGE_PRESENT;

                uint64_t* src_pt = (uint64_t*)((src_pd[ri] & PAGE_ADDR_MASK) + HHDM_OFFSET);
                uint64_t* dst_pt = (uint64_t*)(dst_pt_phys + HHDM_OFFSET);

                for (int si = 0; si < 512; si++) {
                    if (!(src_pt[si] & PAGE_PRESENT)) continue;

                    phys_addr_t src_frame = src_pt[si] & PAGE_ADDR_MASK;
                    uint64_t leaf_flags = src_pt[si] & ~PAGE_ADDR_MASK;

                    // Check if this page is in a shared VMA.
                    virt_addr_t va = ((uint64_t)pi << 39) | ((uint64_t)qi << 30)
                                   | ((uint64_t)ri << 21) | ((uint64_t)si << 12);
                    int is_shared = 0;
                    if (src_mm) {
                        vma_t* vma = mm_vma_find(src_mm, va);
                        if (vma && (vma->shmem || (vma->flags & VMA_MMIO)))
                            is_shared = 1;
                    }

                    if (is_shared) {
                        // Shared: map the SAME physical frame (no copy).
                        dst_pt[si] = src_frame | leaf_flags;
                    } else {
                        // Private: allocate a new frame and deep-copy.
                        phys_addr_t dst_frame = pmm_buddy_alloc(0);
                        if (dst_frame == PMM_INVALID_ADDR) return 0;

                        uint8_t* s = (uint8_t*)(src_frame + HHDM_OFFSET);
                        uint8_t* d = (uint8_t*)(dst_frame + HHDM_OFFSET);
                        for (int b = 0; b < (int)PAGE_SIZE; b++) d[b] = s[b];

                        dst_pt[si] = dst_frame | leaf_flags;
                    }
                }
            }
        }
    }
    return 1;
}

// Legacy wrapper: treats all pages as private (deep-copy everything).
uint8_t vmm_clone_user(phys_addr_t dst_pml4, phys_addr_t src_pml4) {
    return vmm_clone_user_ex(dst_pml4, src_pml4, NULL);
}

// ── Page-fault handler (vector 14) ───────────────────────────────────────
//
// Error code bits:
//   bit 0 (P)  : 0 = not-present,  1 = protection violation
//   bit 1 (W)  : 0 = read,         1 = write
//   bit 2 (U)  : 0 = kernel fault, 1 = user fault
//   bit 3 (R)  : reserved-bit violation (always kill)
//   bit 4 (I)  : 0 = data access,  1 = instruction fetch
//
// User fault handling (demand paging):
//   1. Look up the faulting address in the current task's VMA list.
//   2. If no VMA covers it → segfault (kill task).
//   3. If present-bit fault (protection violation) → segfault.
//   4. If write to a read-only VMA → segfault.
//   5. If instruction fetch on a non-executable VMA → segfault.
//   6. Otherwise: allocate a physical frame, zero it, map it with VMA flags.
//
// Kernel fault handling:
//   Not-present in kernel space → demand-map (kheap expansion).
//   Protection violation in kernel space → panic (unrecoverable).

// Forward declaration — g_current lives in sched.c.
struct task_t;
extern struct task_t* g_current;

static void ser_hex64(uint64_t v) {
    const char* h = "0123456789ABCDEF";
    char buf[19]; buf[0]='0'; buf[1]='x';
    for (int i = 0; i < 16; i++) buf[2+i] = h[(v >> (60 - i*4)) & 0xF];
    buf[18] = '\n';
    for (int i = 0; i < 19; i++) {
        while (!(inb(0x3F8+5) & 0x20));
        outb(0x3F8, (uint8_t)buf[i]);
    }
}
static void ser_str(const char* s) {
    for (; *s; s++) { while (!(inb(0x3F8+5) & 0x20)) {} outb(0x3F8, (uint8_t)*s); }
}

static void kill_current(void) {
    extern void signal_send(struct task_t* t, int sig);
    extern void signal_deliver_pending(void);
    if (g_current) signal_send(g_current, 11); // SIGSEGV = 11
    signal_deliver_pending();
    for (;;) __asm__ volatile("cli; hlt");
}

void isr14_page_fault(interrupt_frame_t* f, uint64_t ec) {
    virt_addr_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    uint8_t is_present  = (ec >> 0) & 1;  // protection violation (not just missing)
    uint8_t is_write    = (ec >> 1) & 1;
    uint8_t is_user     = (ec >> 2) & 1;
    uint8_t is_reserved = (ec >> 3) & 1;
    uint8_t is_ifetch   = (ec >> 4) & 1;
    (void)f;

    // Reserved-bit violation: always unrecoverable.
    if (is_reserved) goto kernel_panic;

    if (is_user || (!is_user && g_current && fault_addr < 0x8000000000000000ULL)) {
        // ── User-mode fault OR kernel touching a user VMA (e.g. sys_getcwd) ─
        extern mm_t* task_get_mm(void* task);
        mm_t* mm = g_current ? task_get_mm(g_current) : NULL;

        if (!mm) goto kernel_panic;

        // Protection violation (present page, wrong permissions).
        // For user faults → kill. For kernel faults → panic (shouldn't happen).
        if (is_present) { if (is_user) goto kill; else goto kernel_panic; }

        vma_t* vma = mm_vma_find(mm, fault_addr);
        if (!vma) { if (is_user) goto kill; else goto kernel_panic; }

        // Write to read-only mapping.
        if (is_write && !(vma->flags & VMA_W)) { if (is_user) goto kill; else goto kernel_panic; }

        // Instruction fetch on non-executable mapping.
        if (is_ifetch && !(vma->flags & VMA_X)) { if (is_user) goto kill; else goto kernel_panic; }

        // Demand-page: resolve the physical frame.
        virt_addr_t page = fault_addr & ~PAGE_MASK;
        phys_addr_t frame;

        if (vma->shmem) {
            // Shared mapping: get/allocate the page from the shmem object.
            // The shmem object owns the physical frame — do NOT free it
            // when unmapping this VMA.
            uint32_t pg_idx = (uint32_t)((page - vma->start) / PAGE_SIZE)
                              + vma->shmem_pgoff;
            frame = shmem_get_page(vma->shmem, pg_idx);
            if (frame == PMM_INVALID_ADDR) {
                if (is_user) goto kill; else goto kernel_panic;
            }
        } else {
            // Private anonymous: allocate a fresh zeroed frame.
            frame = pmm_buddy_alloc(0);
            if (frame == PMM_INVALID_ADDR) {
                if (is_user) goto kill; else goto kernel_panic;
            }
            // Zero the frame (security: never expose old data to user).
            uint64_t* p = (uint64_t*)(frame + HHDM_OFFSET);
            for (int i = 0; i < 512; i++) p[i] = 0;
        }

        if (!vmm_page_map(vmm_pml4_get(), page, frame,
                          mm_vma_pte_flags(vma->flags))) {
            // Only free the frame if it's private (shmem owns shared frames).
            if (!vma->shmem) pmm_buddy_free(frame, 0);
            if (is_user) goto kill; else goto kernel_panic;
        }
        return; // fault resolved

    kill:
        ser_str("PF-KILL ec=");  ser_hex64(ec);
        ser_str("  CR2=");       ser_hex64(fault_addr);
        ser_str("  RIP=");       ser_hex64(f->ip);
        ser_str("  RSP=");       ser_hex64(f->sp);
        ser_str("  p="); ser_hex64(is_present);
        ser_str("  w="); ser_hex64(is_write);
        ser_str("  x="); ser_hex64(is_ifetch);
        if (g_current) {
            extern mm_t* task_get_mm(void*);
            mm_t* dbg_mm = task_get_mm(g_current);
            if (dbg_mm) {
                ser_str("  VMAs:\n");
                for (vma_t* v = dbg_mm->vmas; v; v = v->next) {
                    ser_str("    ["); ser_hex64(v->start);
                    ser_str(" - ");  ser_hex64(v->end);
                    ser_str("] f="); ser_hex64(v->flags);
                }
            }
        }
        // Print top of user stack safely via page table walk
        ser_str("  STACK@RSP:\n");
        {
            uint64_t sp = f->sp;
            for (int i = 0; i < 8; i++) {
                uint64_t va = sp + (uint64_t)(i * 8);
                phys_addr_t pg = vmm_page_phys(vmm_pml4_get(), va & ~0xFFFULL);
                if (pg == PMM_INVALID_ADDR) { ser_str("    (unmapped)\n"); break; }
                uint64_t* kp = (uint64_t*)((pg + HHDM_OFFSET) + (va & 0xFF8ULL));
                ser_str("    "); ser_hex64(va); ser_str(": "); ser_hex64(*kp); ser_str("\n");
            }
        }
        kill_current();
        return;
    }

    // ── Kernel-mode fault ─────────────────────────────────────────────────
    if (!is_present) {
        // Not-present in kernel space: demand-map (kheap expansion).
        phys_addr_t frame = pmm_buddy_alloc(0);
        if (frame == PMM_INVALID_ADDR) goto kernel_panic;
        uint64_t* p = (uint64_t*)(frame + HHDM_OFFSET);
        for (int i = 0; i < 512; i++) p[i] = 0;
        vmm_page_map(g_kernel_pml4, fault_addr & ~PAGE_MASK, frame, VMM_KDATA);
        return;
    }

kernel_panic:
    // Kernel protection violation or reserved-bit fault — unrecoverable.
    for (;;) __asm__ volatile("cli; hlt");
}
