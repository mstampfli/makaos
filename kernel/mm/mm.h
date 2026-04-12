#pragma once
#include "common.h"
#include "vmm.h"

// ── VMA flags (protection bits) ───────────────────────────────────────────
#define VMA_R      (1U << 0)   // readable
#define VMA_W      (1U << 1)   // writable
#define VMA_X      (1U << 2)   // executable
#define VMA_ANON   (1U << 3)   // anonymous (backed by zero pages)
#define VMA_SHARED (1U << 4)   // shared mapping (backed by shmem_t)
#define VMA_MMIO   (1U << 5)   // physical MMIO mapping (not CoW-able, not swappable)
// W^X enforced: kernel refuses to create a VMA with both VMA_W and VMA_X.

// ── Virtual Memory Area ───────────────────────────────────────────────────
// Describes one contiguous region of a process's virtual address space.
// Backed by physical frames allocated on demand (first access triggers #PF).
//
// Linux equivalent: struct vm_area_struct
struct shmem;  // forward declaration — defined in shmem.h

typedef struct vma_t {
    virt_addr_t     start;      // inclusive, page-aligned
    virt_addr_t     end;        // exclusive, page-aligned  (end - start = region size)
    uint32_t        flags;      // VMA_R | VMA_W | VMA_X | VMA_ANON | VMA_SHARED
    struct vma_t*   next;       // intrusive singly-linked list, sorted by start

    // Shared memory backing (NULL for private anonymous VMAs).
    // When non-NULL, physical pages come from the shmem object rather than
    // being privately allocated.  Multiple VMAs (across processes) can point
    // to the same shmem_t.  The shmem's refcount tracks all references.
    struct shmem*   shmem;      // backing shared memory object
    uint32_t        shmem_pgoff;// page offset into the shmem object
} vma_t;

// ── Memory descriptor ─────────────────────────────────────────────────────
// One per address space (PML4).  Shared by all threads in a task group.
// Kernel threads have mm == NULL.
//
// Linux equivalent: struct mm_struct
typedef struct mm_t {
    vma_t*      vmas;           // sorted linked list of VMAs
    virt_addr_t brk_start;      // base of the heap region (fixed at create time)
    virt_addr_t brk;            // current heap top (grows up via sys_brk)
    virt_addr_t mmap_base;      // hint for next anonymous mmap (grows downward)
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

// Clone a mm_t: allocate a new mm_t, copy brk_start/brk, copy VMA list.
// Does NOT copy physical pages (use vmm_clone_user for that).
mm_t* mm_clone(const mm_t* src);

// Remove the VMA that covers exactly [start, end).  Returns 1 if found and
// removed, 0 if no matching VMA exists.  Handles partial unmaps by splitting
// or shrinking the existing VMA.
// Does NOT unmap physical pages — caller must walk page tables first.
uint8_t mm_vma_remove(mm_t* mm, virt_addr_t start, virt_addr_t end);

// Find a free virtual address range of at least `len` bytes (page-aligned)
// in the anonymous mmap region [VMM_MMAP_BASE, VMM_USER_STACK_TOP - stack_size).
// Returns the start address, or 0 if no gap found.
virt_addr_t mm_vma_find_free(mm_t* mm, size_t len);

// mmap/prot flags (POSIX subset)
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10
