#include "vmm.h"
#include "pmm.h"
#include "idt.h"
#include "mm.h"
#include "shmem.h"
#include "vfs.h"
#include "pcache.h"
#include "process.h"
#include "sched.h"
#include "rcu.h"

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
    __builtin_memset((void*)addr, 0, PAGE_SIZE);
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
// Used for the framebuffer.  Pages are mapped write-combining (WC) so the
// CPU batches stores and flushes in bursts — critical for framebuffer perf.
// WC is selected via PWT=1, PCD=0 → PAT entry 1, which kmain programs to WC.
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

    // Map all pages write-combining: PWT=1, PCD=0 → PAT[1]=WC (set in kmain).
    // NX: framebuffer pages are data-only, never executed.
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

    // Lower half = user (256 × 8 = 2 KiB zeroed).
    __builtin_memset(new_pml4, 0, 256 * sizeof(uint64_t));
    // Upper half = shared kernel (copy 2 KiB from the kernel's PML4).
    __builtin_memcpy(new_pml4 + 256, kern_pml4 + 256, 256 * sizeof(uint64_t));

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
// Resolve `count` consecutive pages starting at `uaddr` into HHDM kernel
// pointers.  Handles demand-paging and CoW break: if a page is shared
// (refcount > 1, read-only), we allocate a private copy so DMA writes
// don't corrupt the shared frame.
//
// Returns `count` on success, 0 on any failure (OOM, bad address).
uint32_t vmm_get_user_pages(phys_addr_t pml4_phys, virt_addr_t uaddr,
                            uint32_t count, void** out) {
    uint64_t inter = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t leaf  = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX;

    for (uint32_t i = 0; i < count; i++) {
        virt_addr_t va = (uaddr & ~0xFFFULL) + (uint64_t)i * PAGE_SIZE;

        pte_t* pte = vmm_pte_get(pml4_phys, va, 0, 0);

        if (pte && (*pte & PAGE_PRESENT)) {
            phys_addr_t phys = *pte & PAGE_ADDR_MASK;

            // CoW break: shared page (rc > 1) without write bit → copy.
            if (!(*pte & PAGE_WRITABLE) && pmm_ref_get(phys) > 1) {
                phys_addr_t nf = pmm_buddy_alloc(0);
                if (nf == PMM_INVALID_ADDR) return 0;
                uint8_t* s = (uint8_t*)(phys + HHDM_OFFSET);
                uint8_t* d = (uint8_t*)(nf + HHDM_OFFSET);
                __builtin_memcpy(d, s, PAGE_SIZE);
                *pte = nf | leaf;
                invlpg(va);
                pmm_ref_dec(phys);
                out[i] = (void*)(nf + HHDM_OFFSET);
            } else if (!(*pte & PAGE_WRITABLE) && pmm_ref_get(phys) == 1) {
                // Sole owner, still marked RO from old CoW — re-enable write.
                *pte |= PAGE_WRITABLE;
                invlpg(va);
                out[i] = (void*)(phys + HHDM_OFFSET);
            } else {
                out[i] = (void*)(phys + HHDM_OFFSET);
            }
        } else {
            // Not mapped — demand-allocate.
            phys_addr_t frame = pmm_buddy_alloc(0);
            if (frame == PMM_INVALID_ADDR) return 0;
            uint8_t* p = (uint8_t*)(frame + HHDM_OFFSET);
            __builtin_memset(p, 0, PAGE_SIZE);
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
        pmm_ref_dec(frame);  // CoW-aware: only frees when rc→0
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
                        rcu_read_lock();
                        vma_t* vma = mm_vma_find(mm, va);
                        if (vma && (vma->shmem || (vma->flags & VMA_MMIO)))
                            skip_free = 1;
                        rcu_read_unlock();
                    }

                    if (!skip_free)
                        pmm_ref_dec(leaf);  // CoW-aware: only frees when rc→0
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
//
// Copy-on-Write:
//   Private pages are NOT deep-copied.  Instead, both parent and child PTEs
//   point to the SAME physical frame, both marked read-only.  The frame's
//   refcount is incremented.  The first write by either side triggers a
//   page fault → CoW break (see isr14_page_fault).
//
// Exceptions:
//   - Shared/MMIO VMAs: map the same frame with original flags (no CoW).
//   - Pinned frames (DMA in flight): deep-copy immediately — the parent
//     must keep exclusive ownership so the DMA target isn't corrupted.
//   - src_mm == NULL: legacy deep-copy everything (error-path safety).
//
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

                    // Reconstruct virtual address for VMA lookup.
                    virt_addr_t va = ((uint64_t)pi << 39) | ((uint64_t)qi << 30)
                                   | ((uint64_t)ri << 21) | ((uint64_t)si << 12);

                    // Shared/MMIO VMAs: share frame with original flags (no CoW).
                    int is_shared = 0;
                    if (src_mm) {
                        rcu_read_lock();
                        vma_t* vma = mm_vma_find(src_mm, va);
                        if (vma && (vma->shmem || (vma->flags & VMA_MMIO)))
                            is_shared = 1;
                        rcu_read_unlock();
                    }

                    if (is_shared) {
                        dst_pt[si] = src_frame | leaf_flags;
                        continue;
                    }

                    // No mm → legacy deep-copy (error-path safety).
                    // Pinned frame → deep-copy (DMA in flight, parent keeps exclusive).
                    if (!src_mm || pmm_pin_get(src_frame) > 0) {
                        phys_addr_t dst_frame = pmm_buddy_alloc(0);
                        if (dst_frame == PMM_INVALID_ADDR) return 0;

                        uint8_t* s = (uint8_t*)(src_frame + HHDM_OFFSET);
                        uint8_t* d = (uint8_t*)(dst_frame + HHDM_OFFSET);
                        __builtin_memcpy(d, s, PAGE_SIZE);

                        dst_pt[si] = dst_frame | leaf_flags;
                        continue;
                    }

                    // ── CoW: share the frame, mark BOTH read-only ────────
                    // Clear RW bit in both parent and child leaf PTEs.
                    // The frame's refcount is bumped to track sharing.
                    uint64_t cow_flags = leaf_flags & ~PAGE_WRITABLE;

                    src_pt[si] = src_frame | cow_flags;   // parent: now read-only
                    dst_pt[si] = src_frame | cow_flags;   // child:  same frame, read-only

                    pmm_ref_inc(src_frame);               // shared: rc 1→2
                }
            }
        }
    }

    // Flush parent's TLB — we changed its PTEs from RW to RO.
    // If src_pml4 is the currently loaded one, we must flush.
    if (vmm_pml4_get() == src_pml4) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(src_pml4) : "memory");
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

// g_current is a per-CPU accessor macro from sched.h (expands to
// this_cpu()->current).  The page-fault handler runs in process
// context with preempt disabled at ISR entry, so the per-CPU read is
// stable.

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

        // ── Copy-on-Write break ──────────────────────────────────────────
        // Protection violation on write to a present page: check for CoW.
        // CoW pages are present + read-only, in a writable VMA, with rc > 1.
        if (is_present && is_write) {
            uint32_t vma_flags = 0;
            int have_vma = 0;
            rcu_read_lock();
            {
                vma_t* vma = mm_vma_find(mm, fault_addr);
                if (vma && (vma->flags & VMA_W) && !vma->shmem && !(vma->flags & VMA_MMIO)) {
                    vma_flags = vma->flags;
                    have_vma = 1;
                }
            }
            rcu_read_unlock();

            if (have_vma) {
                virt_addr_t page = fault_addr & ~PAGE_MASK;
                phys_addr_t old_frame = vmm_page_phys(vmm_pml4_get(), page);
                if (old_frame != PMM_INVALID_ADDR) {
                    uint32_t rc = pmm_ref_get(old_frame);
                    if (rc > 1) {
                        // Shared CoW page — allocate a private copy.
                        phys_addr_t new_frame = pmm_buddy_alloc(0);
                        if (new_frame == PMM_INVALID_ADDR) {
                            if (is_user) goto kill; else goto kernel_panic;
                        }
                        uint8_t* s = (uint8_t*)(old_frame + HHDM_OFFSET);
                        uint8_t* d = (uint8_t*)(new_frame + HHDM_OFFSET);
                        __builtin_memcpy(d, s, PAGE_SIZE);

                        vmm_page_map(vmm_pml4_get(), page, new_frame,
                                     mm_vma_pte_flags(vma_flags));
                        pmm_ref_dec(old_frame);
                        return; // fault resolved
                    }
                    if (rc == 1) {
                        pte_t* pte = vmm_pte_get(vmm_pml4_get(), page, 0, 0);
                        if (pte) {
                            *pte |= PAGE_WRITABLE;
                            invlpg(page);
                        }
                        return; // fault resolved
                    }
                }
            }
            // Not a CoW page — genuine protection violation, fall through to kill.
        }

        // Protection violation (present page, not CoW).
        if (is_present) { if (is_user) goto kill; else goto kernel_panic; }

        // Snapshot the VMA fields we need under RCU so we can drop the
        // reader section before doing any physical work.  shmem_tryget
        // handles the case where shmem_unref races our use.
        uint32_t       vma_flags = 0;
        virt_addr_t    vma_start = 0;
        uint32_t       vma_shmem_pgoff = 0;
        struct shmem*  vma_shmem = NULL;
        vfs_file_t*    vma_file  = NULL;
        uint64_t       vma_file_off = 0;
        uint64_t       vma_file_len = 0;
        int            have_vma = 0;
        rcu_read_lock();
        {
            vma_t* vma = mm_vma_find(mm, fault_addr);
            if (vma) {
                vma_flags       = vma->flags;
                vma_start       = vma->start;
                vma_shmem_pgoff = vma->shmem_pgoff;
                if (vma->shmem && shmem_tryget(vma->shmem))
                    vma_shmem = vma->shmem;
                if ((vma->flags & VMA_FILE) && vma->file)
                    vma_file = vfs_tryget(vma->file);
                vma_file_off = vma->file_off;
                vma_file_len = vma->file_len;
                have_vma = 1;
            }
        }
        rcu_read_unlock();

        if (!have_vma) { if (is_user) goto kill; else goto kernel_panic; }
        if (is_write  && !(vma_flags & VMA_W)) {
            if (vma_shmem) shmem_unref(vma_shmem);
            if (vma_file)  vfs_close(vma_file);
            if (is_user) goto kill; else goto kernel_panic;
        }
        if (is_ifetch && !(vma_flags & VMA_X)) {
            if (vma_shmem) shmem_unref(vma_shmem);
            if (vma_file)  vfs_close(vma_file);
            if (is_user) goto kill; else goto kernel_panic;
        }

        // Demand-page: resolve the physical frame.
        virt_addr_t page = fault_addr & ~PAGE_MASK;
        phys_addr_t frame;

        if (vma_file) {
            // ── File-backed page with page cache ──────────────────────────
            // Enable IRQs before any blocking disk read.  AHCI is IRQ-driven;
            // iretq restores user RFLAGS (IF=1) on return, so no cli needed.
            __asm__ volatile("sti");

            // pg_file_idx: index of this page within the backing file.
            // vma_file_off is page-aligned (elf_load_into rounds down), so
            // the shift is exact.
            uint64_t pg_off      = (uint64_t)(page - vma_start);
            uint32_t pg_file_idx = (uint32_t)((vma_file_off + pg_off) >> PAGE_SHIFT);
            uint32_t ino         = vma_file->ino;

            // clean_frame: on-disk page content (from cache or fresh read).
            // clean_in_cache == 1 → cache holds a ref; caller must pmm_ref_inc
            //                       before installing as a PTE.
            // clean_in_cache == 0 → alloc ref IS the only ref (no inc needed
            //                       for a sole-owner PTE, but must pmm_ref_dec
            //                       if we're making a private copy instead).
            phys_addr_t clean_frame;
            uint8_t     clean_in_cache;

            clean_frame = (ino != 0) ? pcache_get(ino, pg_file_idx)
                                     : PMM_INVALID_ADDR;

            if (clean_frame != PMM_INVALID_ADDR) {
                // Cache hit — no disk I/O.
                clean_in_cache = 1;
                if (g_current)
                    __atomic_fetch_add(&g_current->pf_cache, 1, __ATOMIC_RELAXED);
            } else {
                // Cache miss: read from disk.
                if (g_current)
                    __atomic_fetch_add(&g_current->pf_disk, 1, __ATOMIC_RELAXED);
                clean_frame = pmm_buddy_alloc(0);
                if (clean_frame == PMM_INVALID_ADDR) {
                    vfs_close(vma_file);
                    if (is_user) goto kill; else goto kernel_panic;
                }
                uint8_t* dst = (uint8_t*)(clean_frame + HHDM_OFFSET);
                __builtin_memset(dst, 0, PAGE_SIZE);
                if (pg_off < vma_file_len) {
                    uint64_t src_off = vma_file_off + pg_off;
                    uint64_t bytes   = PAGE_SIZE;
                    if (pg_off + bytes > vma_file_len)
                        bytes = vma_file_len - pg_off;
                    vma_file->seek(vma_file, (int64_t)src_off, 0 /*SEEK_SET*/);
                    vma_file->read(vma_file, dst, bytes);
                }

                if (ino != 0) {
                    // Offer to cache.  pcache_insert takes the alloc ref and
                    // returns: our frame (inserted) or a racer's frame (ours
                    // freed).  PMM_INVALID_ADDR means OOM on entry node.
                    phys_addr_t c = pcache_insert(ino, pg_file_idx, clean_frame);
                    if (c != PMM_INVALID_ADDR) {
                        clean_frame    = c;
                        clean_in_cache = 1;
                    } else {
                        // Entry OOM: use frame directly, not cached.
                        clean_in_cache = 0;
                    }
                } else {
                    clean_in_cache = 0;
                }

                // ── Read-ahead: prefetch pages N+1..N+7 into pcache ──────────
                // After a disk miss we pre-fetch the next RA_PAGES consecutive
                // file pages through the same VFS handle (still open here).
                // Each goes through ext2→ahci_read as a normal NCQ command so
                // the HBA sees multiple queued reads and can reorder them for
                // optimal rotational latency.  Future faults on those pages
                // are then 100% cache hits — no additional disk I/O.
                //
                // We only do read-ahead when:
                //   (a) the file has an inode (ino != 0, ext2-backed), and
                //   (b) vma_file_len tells us there is more file data ahead.
                // If OOM, we stop early (non-fatal — the missed pages will just
                // fault from disk later).  We skip pages already in cache so a
                // sequential fault stream doesn't double-read.
                if (ino != 0) {
#define RA_PAGES 7
                    for (uint32_t ra = 1; ra <= RA_PAGES; ra++) {
                        uint64_t ra_pg_off = pg_off + (uint64_t)ra * PAGE_SIZE;
                        if (ra_pg_off >= vma_file_len) break;
                        uint32_t ra_pg_idx = pg_file_idx + ra;
                        if (pcache_get(ino, ra_pg_idx) != PMM_INVALID_ADDR)
                            continue;  // already cached, skip
                        phys_addr_t ra_frame = pmm_buddy_alloc(0);
                        if (ra_frame == PMM_INVALID_ADDR) break;  // OOM
                        uint8_t* ra_dst = (uint8_t*)(ra_frame + HHDM_OFFSET);
                        __builtin_memset(ra_dst, 0, PAGE_SIZE);
                        uint64_t ra_src_off = vma_file_off + ra_pg_off;
                        uint64_t ra_bytes   = PAGE_SIZE;
                        if (ra_pg_off + ra_bytes > vma_file_len)
                            ra_bytes = vma_file_len - ra_pg_off;
                        vma_file->seek(vma_file, (int64_t)ra_src_off, 0);
                        vma_file->read(vma_file, ra_dst, ra_bytes);
                        // pcache_insert: on success cache owns the alloc ref.
                        // On race (another CPU just inserted same page):
                        //   returns existing frame; our ra_frame is freed.
                        // On OOM for the entry node:
                        //   returns PMM_INVALID_ADDR; we must free ra_frame.
                        phys_addr_t ra_c = pcache_insert(ino, ra_pg_idx, ra_frame);
                        if (ra_c == PMM_INVALID_ADDR)
                            pmm_ref_dec(ra_frame);  // OOM on entry alloc
                        // else: ra_c owns the ref (either our frame or racer's)
                    }
#undef RA_PAGES
                }
            }
            vfs_close(vma_file);

            // RO segments (text/rodata): share clean_frame directly.
            // RW segments (data/BSS):    private copy — cache keeps clean.
            if (vma_flags & VMA_W) {
                frame = pmm_buddy_alloc(0);
                if (frame == PMM_INVALID_ADDR) {
                    if (!clean_in_cache) pmm_ref_dec(clean_frame);
                    if (is_user) goto kill; else goto kernel_panic;
                }
                __builtin_memcpy((void*)(frame       + HHDM_OFFSET),
                                 (void*)(clean_frame + HHDM_OFFSET),
                                 PAGE_SIZE);
                if (!clean_in_cache) pmm_ref_dec(clean_frame);
                // frame: rc == 1 (alloc ref) → PTE ref.
            } else {
                // RO: reuse clean_frame as the PTE frame.
                if (clean_in_cache)
                    pmm_ref_inc(clean_frame); // +1 for PTE; cache keeps its own.
                // else: rc == 1 (alloc ref) → PTE ref directly.
                frame = clean_frame;
            }

        } else if (vma_shmem) {
            uint32_t pg_idx = (uint32_t)((page - vma_start) / PAGE_SIZE)
                              + vma_shmem_pgoff;
            frame = shmem_get_page(vma_shmem, pg_idx);
            if (frame == PMM_INVALID_ADDR) {
                shmem_unref(vma_shmem);
                if (is_user) goto kill; else goto kernel_panic;
            }
        } else {
            frame = pmm_buddy_alloc(0);
            if (frame == PMM_INVALID_ADDR) {
                if (is_user) goto kill; else goto kernel_panic;
            }
            __builtin_memset((void*)(frame + HHDM_OFFSET), 0, PAGE_SIZE);
        }

        if (!vmm_page_map(vmm_pml4_get(), page, frame,
                          mm_vma_pte_flags(vma_flags))) {
            // Free the frame we prepared but couldn't map.
            // shmem frames are owned by the shmem object; anon/file frames
            // are PMM-ref-counted (rc == 1 here → pmm_ref_dec frees them).
            if (vma_shmem) shmem_unref(vma_shmem);
            else pmm_ref_dec(frame);
            if (is_user) goto kill; else goto kernel_panic;
        }
        if (vma_shmem) shmem_unref(vma_shmem);
        return; // fault resolved

    kill:
    {
        // ── Header ────────────────────────────────────────────────────────
        ser_str("\n===== PF-KILL =====\n");
        // pid / comm: page fault handler runs in process context with
        // IRQs enabled but preempt disabled at ISR entry, so
        // this_cpu()->current is stable across the print path.
        if (g_current) {
            ser_str("  pid="); ser_hex64((uint64_t)g_current->pid);
            ser_str("  ppid="); ser_hex64((uint64_t)g_current->ppid);
            ser_str("  home=");ser_hex64((uint64_t)g_current->home_cpu);
            ser_str("  comm=");
            for (int i = 0; i < 15 && g_current->comm[i]; i++) {
                while (!(inb(0x3F8+5) & 0x20));
                outb(0x3F8, (uint8_t)g_current->comm[i]);
            }
            ser_str("\n");
        }
        ser_str("  ec=");   ser_hex64(ec);
        ser_str("  CR2=");  ser_hex64(fault_addr);
        ser_str("  RIP=");  ser_hex64(f->ip);
        ser_str("  RSP=");  ser_hex64(f->sp);
        ser_str("  RFLAGS=");ser_hex64(f->flags);
        ser_str("  CS=");   ser_hex64(f->cs);
        ser_str("  p=");ser_hex64(is_present);
        ser_str("  w=");ser_hex64(is_write);
        ser_str("  x=");ser_hex64(is_ifetch);

        // ── User GPRs ─────────────────────────────────────────────────────
        // The ISR stub pushed 15 GPRs before calling us, then sub'd rsp
        // by 40 for the synthetic interrupt_frame_t.  So the GPRs live
        // at (uint64_t*)f + 5 .. +19, in the reverse of push order:
        //    +5 rax, +6 rbx, +7 rcx, +8 rdx, +9 rbp, +10 rsi, +11 rdi,
        //    +12 r8,  +13 r9,  +14 r10, +15 r11, +16 r12, +17 r13,
        //    +18 r14, +19 r15.
        // See kernel/arch/x86_64/isr_stubs.asm PUSH_GPRS.
        {
            uint64_t* g = (uint64_t*)((uintptr_t)f + 5 * 8);
            ser_str("  RAX="); ser_hex64(g[0]);
            ser_str("  RBX="); ser_hex64(g[1]);
            ser_str("  RCX="); ser_hex64(g[2]);
            ser_str("  RDX="); ser_hex64(g[3]);
            ser_str("  RBP="); ser_hex64(g[4]);
            ser_str("  RSI="); ser_hex64(g[5]);
            ser_str("  RDI="); ser_hex64(g[6]);
            ser_str("  R8="); ser_hex64(g[7]);
            ser_str("  R9="); ser_hex64(g[8]);
            ser_str("  R10=");ser_hex64(g[9]);
            ser_str("  R11=");ser_hex64(g[10]);
            ser_str("  R12=");ser_hex64(g[11]);
            ser_str("  R13=");ser_hex64(g[12]);
            ser_str("  R14=");ser_hex64(g[13]);
            ser_str("  R15=");ser_hex64(g[14]);
        }

        // ── VMAs ──────────────────────────────────────────────────────────
        if (g_current) {
            extern mm_t* task_get_mm(void*);
            mm_t* dbg_mm = task_get_mm(g_current);
            if (dbg_mm) {
                ser_str("  VMAs:\n");
                rcu_read_lock();
                for (vma_t* v = rcu_dereference(dbg_mm->vmas); v;
                             v = rcu_dereference(v->next)) {
                    ser_str("    ["); ser_hex64(v->start);
                    ser_str(" - ");  ser_hex64(v->end);
                    ser_str("] f="); ser_hex64(v->flags);
                    // Is the fault addr inside this VMA?
                    if (fault_addr >= v->start && fault_addr < v->end) {
                        ser_str("    ^ fault addr inside, offset=");
                        ser_hex64(fault_addr - v->start);
                    }
                }
                rcu_read_unlock();
            }
        }

        // ── User stack dump ───────────────────────────────────────────────
        ser_str("  STACK@RSP (32 words):\n");
        {
            uint64_t sp = f->sp;
            for (int i = 0; i < 32; i++) {
                uint64_t va = sp + (uint64_t)(i * 8);
                phys_addr_t pg = vmm_page_phys(vmm_pml4_get(), va & ~0xFFFULL);
                if (pg == PMM_INVALID_ADDR) { ser_str("    (unmapped)\n"); break; }
                uint64_t* kp = (uint64_t*)((pg + HHDM_OFFSET) + (va & 0xFF8ULL));
                ser_str("    "); ser_hex64(va); ser_str(": "); ser_hex64(*kp);
            }
        }

        // ── Instruction bytes at faulting RIP ─────────────────────────────
        ser_str("  CODE@RIP:\n");
        {
            uint64_t ip = f->ip;
            phys_addr_t pg = vmm_page_phys(vmm_pml4_get(), ip & ~0xFFFULL);
            if (pg == PMM_INVALID_ADDR) {
                ser_str("    (RIP not mapped)\n");
            } else {
                uint64_t* kp = (uint64_t*)((pg + HHDM_OFFSET) + (ip & 0xFF8ULL));
                ser_str("    "); ser_hex64(kp[0]);
                ser_str("    "); ser_hex64(kp[1]);
            }
        }
        ser_str("===== END PF-KILL =====\n\n");
        kill_current();
        return;
    }
    }

    // ── Kernel-mode fault ─────────────────────────────────────────────────
    if (!is_present) {
        // Not-present in kernel space: demand-map (kheap expansion).
        phys_addr_t frame = pmm_buddy_alloc(0);
        if (frame == PMM_INVALID_ADDR) goto kernel_panic;
        __builtin_memset((void*)(frame + HHDM_OFFSET), 0, PAGE_SIZE);
        vmm_page_map(g_kernel_pml4, fault_addr & ~PAGE_MASK, frame, VMM_KDATA);
        return;
    }

kernel_panic:
    // Kernel protection violation or reserved-bit fault — unrecoverable.
    for (;;) __asm__ volatile("cli; hlt");
}
