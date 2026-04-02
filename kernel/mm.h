#pragma once
#include "common.h"
#include "vmm.h"

// ── VMA flags (protection bits) ───────────────────────────────────────────
#define VMA_R    (1U << 0)   // readable
#define VMA_W    (1U << 1)   // writable
#define VMA_X    (1U << 2)   // executable
#define VMA_ANON (1U << 3)   // anonymous (backed by zero pages)
// W^X enforced: kernel refuses to create a VMA with both VMA_W and VMA_X.

// ── Virtual Memory Area ───────────────────────────────────────────────────
// Describes one contiguous region of a process's virtual address space.
// Backed by physical frames allocated on demand (first access triggers #PF).
//
// Linux equivalent: struct vm_area_struct
typedef struct vma_t {
    virt_addr_t     start;      // inclusive, page-aligned
    virt_addr_t     end;        // exclusive, page-aligned  (end - start = region size)
    uint32_t        flags;      // VMA_R | VMA_W | VMA_X | VMA_ANON
    struct vma_t*   next;       // intrusive singly-linked list, sorted by start
} vma_t;

// ── Memory descriptor ─────────────────────────────────────────────────────
// One per address space (PML4).  Shared by all threads in a task group.
// Kernel threads have mm == NULL.
//
// Linux equivalent: struct mm_struct
typedef struct {
    vma_t*      vmas;           // sorted linked list of VMAs
    virt_addr_t brk_start;      // base of the heap region (fixed at create time)
    virt_addr_t brk;            // current heap top (grows up via sys_brk)
    uint32_t    refcount;       // threads sharing this mm (unused until threading)
} mm_t;

// ── API ───────────────────────────────────────────────────────────────────

// Allocate and zero-init a new mm_t.
mm_t* mm_create(void);

// Add a VMA to mm.  Regions must not overlap.
// Returns 1 on success, 0 on overlap or alloc failure.
uint8_t mm_vma_add(mm_t* mm, virt_addr_t start, virt_addr_t end, uint32_t flags);

// Look up the VMA containing `addr`.  Returns NULL if none.
vma_t* mm_vma_find(mm_t* mm, virt_addr_t addr);

// Remove and free all VMAs and the mm_t itself.
// Does NOT unmap or free physical frames — caller must walk the page tables
// via vmm_free_user() first.
void mm_destroy(mm_t* mm);

// Convert VMA flags → PTE flags for vmm_page_map.
uint64_t mm_vma_pte_flags(uint32_t vma_flags);
