#pragma once
#include "common.h"

typedef uint64_t pte_t;

// ── Page Table Entry Bits ─────────────────────────────────────────────────
// Each entry (PML4E, PDPTE, PDE, PTE) is 64 bits:
//   bit 63     : NX  – No-Execute (instruction fetch blocked). Requires NXE in EFER.
//   bits 51:12 : physical address of the next table or the mapped page frame
//   bit  8     : G   – Global (TLB survives CR3 writes; unused here)
//   bit  7     : PS  – Page Size (makes this entry a 2MiB/1GiB huge page)
//   bit  6     : D   – Dirty (CPU sets when the page is written; leaf only)
//   bit  5     : A   – Accessed (CPU sets when the page is read)
//   bit  4     : PCD – Page-level Cache Disable
//   bit  3     : PWT – Page-level Write-Through
//   bit  2     : US  – User/Supervisor: 0 = kernel-only, 1 = ring-3 accessible
//   bit  1     : RW  – Read/Write: 0 = read-only, 1 = writable
//   bit  0     : P   – Present: entry is valid

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)   // ring-3 can access
#define PAGE_PWT      (1ULL << 3)
#define PAGE_PCD      (1ULL << 4)
#define PAGE_ACCESSED (1ULL << 5)
#define PAGE_DIRTY    (1ULL << 6)
#define PAGE_PS       (1ULL << 7)   // huge page (2MiB at PD, 1GiB at PDPT)
#define PAGE_GLOBAL   (1ULL << 8)
#define PAGE_NX       (1ULL << 63)  // no-execute; must enable NXE in EFER first!

#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL

// ── Convenience combinations ──────────────────────────────────────────────
// Kernel code  : P|RW          → kernel r/w, executable (no NX, no US)
// Kernel data  : P|RW|NX       → kernel r/w, NOT executable
// User code    : P|RW|US       → ring-3 r/w, executable (no NX)
// User data    : P|RW|US|NX    → ring-3 r/w, NOT executable
// User rodata  : P|US|NX       → ring-3 read-only, NOT executable
#define VMM_KCODE   (PAGE_PRESENT | PAGE_WRITABLE)
#define VMM_KDATA   (PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX)
#define VMM_UCODE   (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)
#define VMM_UDATA   (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX)
#define VMM_URODATA (PAGE_PRESENT | PAGE_USER | PAGE_NX)

#define VMM_INVALID UINT64_MAX

// ── User address space layout ─────────────────────────────────────────────
// Just a convention — nothing enforces these at hardware level.
#define VMM_USER_CODE_BASE   0x0000000000400000ULL   // code/text at 4MiB
#define VMM_USER_STACK_TOP   0x00007FFFFFFFE000ULL   // stack grows DOWN from here
#define VMM_USER_STACK_PAGES 512ULL                  // 512 × 4KiB = 2MiB stack
// Anonymous mmap region: sits between heap and stack.
// mmap allocations grow downward from this hint.
#define VMM_MMAP_BASE        0x00007FF000000000ULL   // top of mmap region

// ── API ───────────────────────────────────────────────────────────────────

// Call once after PMM is ready. Stores the kernel PML4 phys for cloning.
void vmm_init(phys_addr_t kernel_pml4_phys);

// Create a fresh PML4 for a new process:
//   lower half (PML4[0..255])  : zeroed  – process gets an empty user space
//   upper half (PML4[256..511]): copied from kernel PML4 – all processes share
//                                the same kernel/HHDM mappings automatically
phys_addr_t vmm_alloc_pml4(void);

// Map one 4KiB page: virt → phys, with the given flags.
// Use VMM_UDATA/VMM_UCODE etc. for flags.
uint8_t vmm_page_map(phys_addr_t pml4_phys, virt_addr_t vaddr,
                     phys_addr_t paddr, uint64_t flags);

// Unmap one 4KiB page. Writes the physical address to *out_paddr if not NULL.
// Does NOT free the physical page frame — caller's responsibility.
uint8_t vmm_page_unmap(phys_addr_t pml4_phys, virt_addr_t vaddr,
                       phys_addr_t* out_paddr);

// Walk the page table and return the physical address mapped at vaddr.
// Returns PMM_INVALID_ADDR if not mapped.
phys_addr_t vmm_page_phys(phys_addr_t pml4_phys, virt_addr_t vaddr);

// Allocate a physical frame, map it at vaddr in the CURRENT address space.
void* vmm_page_alloc(virt_addr_t vaddr, uint64_t flags);

// Unmap and free the frame at vaddr in the CURRENT address space.
void vmm_page_free(virt_addr_t vaddr);

// Switch address space: writes pml4_phys to CR3, flushing the TLB.
void vmm_switch(phys_addr_t pml4_phys);

// Free all lower-half (user-space) page table structures and leaf frames.
// Does NOT free the PML4 frame itself — caller frees it.
// Legacy version: frees ALL leaf frames unconditionally (safe for error paths
// where no shmem objects could exist).
void vmm_free_user(phys_addr_t pml4_phys);

// VMA-aware version: skips freeing leaf frames for shared VMAs (shmem-backed).
// Pass mm=NULL for the legacy "free everything" behavior.
struct mm_t;
void vmm_free_user_ex(phys_addr_t pml4_phys, struct mm_t* mm);

// Return the physical address of the kernel's own PML4 (set by vmm_init).
// Use this when you need to map pages into the kernel address space
// regardless of which process is currently running (current CR3 may differ).
phys_addr_t vmm_kernel_pml4_get(void);

// Map a physical MMIO region into kernel virtual space with cache-disabled flags.
// Returns the virtual address to use for accessing the device registers.
// Successive calls allocate from a growing window starting at 0xFFFF900000000000.
virt_addr_t vmm_map_mmio(phys_addr_t phys, uint64_t bytes);

// Map a contiguous range of physical addresses into a process's user
// address space.  Used for the framebuffer (SYS_FB_MAP).
// Creates a VMA with VMA_MMIO flag.  Pages are mapped immediately (not
// demand-paged) with write-combining cache policy (PAT[1]=WC via PWT=1,PCD=0).
// Returns the virtual address, or 0 on failure.
struct mm_t;
virt_addr_t vmm_map_physical_user(struct mm_t* mm, phys_addr_t pml4_phys,
                                  phys_addr_t phys, uint64_t bytes);

// Page-fault ISR (called from IDT handler for vector 14)
typedef struct interrupt_frame_t interrupt_frame_t;
void isr14_page_fault(interrupt_frame_t* f, uint64_t ec);

// ── get_user_pages ───────────────────────────────────────────────────────
// Resolve `count` pages starting at user virtual address `uaddr` to
// kernel-accessible (HHDM) pointers.  Handles demand-paging: if a page
// is not yet mapped, allocates a fresh frame and maps it.
// `out` must have room for `count` pointers.
// Returns `count` on success, 0 on failure.
// The caller must be in the context of the process that owns `uaddr`
// (i.e. the process's PML4 must be the currently loaded one, or
// pml4_phys must be explicitly passed).
uint32_t vmm_get_user_pages(phys_addr_t pml4_phys, virt_addr_t uaddr,
                            uint32_t count, void** out);

// Return the physical address currently in CR3.
phys_addr_t vmm_current_pml4(void);

// Deep-copy all user (lower half, PML4[0..255]) pages from src_pml4 to dst_pml4.
// Allocates new PDPT/PD/PT frames and new data frames for each present leaf PTE.
// Returns 1 on success, 0 on OOM.
uint8_t vmm_clone_user(phys_addr_t dst_pml4, phys_addr_t src_pml4);

// VMA-aware clone: shared (shmem-backed) pages map the same physical frame
// instead of being deep-copied.  Pass src_mm=NULL for legacy deep-copy behavior.
uint8_t vmm_clone_user_ex(phys_addr_t dst_pml4, phys_addr_t src_pml4,
                          struct mm_t* src_mm);
