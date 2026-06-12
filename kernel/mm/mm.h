#pragma once
#include "common.h"
#include "vmm.h"
#include "smp.h"
#include "rcu.h"   // rcu_head_t — vmas are freed via async call_rcu_head

// ── VMA flags (protection bits) ───────────────────────────────────────────
#define VMA_R      (1U << 0)   // readable
#define VMA_W      (1U << 1)   // writable
#define VMA_X      (1U << 2)   // executable
#define VMA_ANON   (1U << 3)   // anonymous (backed by zero pages)
#define VMA_SHARED (1U << 4)   // shared mapping (backed by shmem_t)
#define VMA_MMIO   (1U << 5)   // physical MMIO mapping (not CoW-able, not swappable)
#define VMA_FILE   (1U << 6)   // file-backed mapping (lazy ELF load; demand-paged from disk)
// W^X enforced: kernel refuses to create a VMA with both VMA_W and VMA_X.

// ── Virtual Memory Area ───────────────────────────────────────────────────
// Describes one contiguous region of a process's virtual address space.
// Backed by physical frames allocated on demand (first access triggers #PF).
//
// Linux equivalent: struct vm_area_struct
struct shmem;     // forward declaration — defined in shmem.h
struct vfs_file_t; // forward declaration — defined in vfs.h

typedef struct vma_t {
    virt_addr_t     start;      // inclusive, page-aligned
    virt_addr_t     end;        // exclusive, page-aligned  (end - start = region size)
    uint32_t        flags;      // VMA_R | VMA_W | VMA_X | VMA_ANON | VMA_SHARED | VMA_FILE
    struct vma_t*   next;       // intrusive singly-linked list, sorted by start

    // Shared memory backing (NULL for private anonymous VMAs).
    struct shmem*   shmem;      // backing shared memory object (VMA_SHARED)
    uint32_t        shmem_pgoff;// page offset into the shmem object

    // File backing (VMA_FILE only).  `file` is ref-counted via vfs_dup/vfs_close.
    // On page fault: read PAGE_SIZE bytes from file at (file_off + page_in_vma).
    // Bytes beyond file_len (the BSS tail) are zero-filled.
    struct vfs_file_t* file;     // open file handle (one ref per VMA)
    uint64_t           file_off; // file offset corresponding to vma->start
    uint64_t           file_len; // bytes in file backing this VMA (rest = zero/BSS)

    // Async deferred-free hook.  vmas are unlinked under mm->vma_lock and
    // freed via call_rcu_head AFTER a grace period — NOT via the synchronous
    // call_rcu_expedited (which IPIs every CPU and waits, and would deadlock
    // when a sibling CPU is page-faulting and spinning on this same vma_lock
    // with IRQs off).  See vma_free_rcu / mm_vma_remove.
    rcu_head_t      rcu_head;
} vma_t;

// ── Memory descriptor ─────────────────────────────────────────────────────
// One per address space (PML4).  Shared by all threads in a task group.
// Kernel threads have mm == NULL.
//
// Linux equivalent: struct mm_struct
//
// Concurrency model:
//   - `vmas` is an RCU-protected singly-linked list.  Readers walk it
//     inside rcu_read_lock() using rcu_dereference on each ->next.  VMA
//     descriptors are freed via call_rcu so a concurrent mm_vma_remove
//     cannot yank a VMA out from under a page-fault walker.
//   - Writers (mm_vma_add, mm_vma_remove, mm_vma_find_free, mm_clone's
//     src walk, brk shrink/grow) serialise on `vma_lock`.  The lock is
//     NOT IRQ-off: page fault handlers never take it, they only read
//     under RCU.
//   - brk / brk_start / mmap_base are updated under vma_lock so they
//     stay coherent with the VMA list.
typedef struct mm_t {
    vma_t*      vmas;           // RCU-published sorted linked list of VMAs
    virt_addr_t brk_start;      // base of the heap region (fixed at create time)
    virt_addr_t brk;            // current heap top (grows up via sys_brk)
    virt_addr_t mmap_base;      // hint for next anonymous mmap (grows downward)
    uint32_t    refcount;       // threads sharing this mm (unused until threading)
    spinlock_t  vma_lock;       // writer-side mutex for the VMA list
} mm_t;

// ── API ───────────────────────────────────────────────────────────────────

// Allocate and zero-init a new mm_t.
mm_t* mm_create(void);

// Add an anonymous VMA to mm.  Regions must not overlap.
// Returns 1 on success, 0 on overlap or alloc failure.
uint8_t mm_vma_add(mm_t* mm, virt_addr_t start, virt_addr_t end, uint32_t flags);

// Add a file-backed VMA (VMA_FILE).  `file` is dup'd — caller retains its
// own reference and must still call vfs_close when done with it.
// `file_off` is the file byte offset for vma_start; `file_len` is the number
// of file-backed bytes (pages beyond this offset are zero-filled, i.e. BSS).
// Returns 1 on success, 0 on failure.
uint8_t mm_vma_add_file(mm_t* mm, virt_addr_t start, virt_addr_t end,
                         uint32_t prot_flags,
                         struct vfs_file_t* file,
                         uint64_t file_off, uint64_t file_len);

// Look up the VMA containing `addr`.  Returns NULL if none.
//
// CALLER MUST BE INSIDE rcu_read_lock() and must stay there until it is
// done dereferencing the returned pointer.  The returned VMA is valid
// for the duration of the reader section because mm_vma_remove defers
// the kfree via call_rcu.  The caller may sample v->flags, v->shmem,
// v->shmem_pgoff inside the section — once rcu_read_unlock has fired,
// the pointer must not be touched again.
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

// call_rcu callback for freeing a VMA.  Exposed so callers outside mm.c
// that unlink a VMA directly (under mm->vma_lock) can hand it off for
// deferred reclamation via call_rcu(vma_free_rcu, v).
void vma_free_rcu(void* data);

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
