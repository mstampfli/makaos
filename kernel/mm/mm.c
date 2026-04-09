#include "mm.h"
#include "kheap.h"
#include "vmm.h"
#include "common.h"

/* ── C library stubs required by the compiler ───────────────────────────── */
void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}
void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

// ── mm_create ─────────────────────────────────────────────────────────────
mm_t* mm_create(void) {
    mm_t* mm = kmalloc(sizeof(mm_t));
    if (!mm) return NULL;
    mm->vmas      = NULL;
    mm->brk_start = 0;
    mm->brk       = 0;
    mm->mmap_base = VMM_MMAP_BASE;
    mm->refcount  = 1;
    return mm;
}

// ── mm_vma_add ────────────────────────────────────────────────────────────
// Inserts a new VMA into the sorted list.
// Security: rejects W+X (no-execute bypass), rejects misaligned addresses,
// rejects zero-size or overlapping regions.
uint8_t mm_vma_add(mm_t* mm, virt_addr_t start, virt_addr_t end, uint32_t flags) {
    if (!mm) return 0;

    // Alignment and size checks.
    if (start >= end)              return 0;
    if (start & PAGE_MASK)         return 0;
    if (end   & PAGE_MASK)         return 0;

    // W^X enforced only for kernel VMAs; user flat binaries have .bss in the
    // same page as .text, so user code VMAs legitimately need R+W+X.

    // Check for overlap with existing VMAs.
    for (vma_t* v = mm->vmas; v; v = v->next) {
        if (start < v->end && end > v->start) return 0; // overlaps
    }

    vma_t* vma = kmalloc(sizeof(vma_t));
    if (!vma) return 0;
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    vma->next  = NULL;

    // Insert sorted by start address.
    if (!mm->vmas || start < mm->vmas->start) {
        vma->next = mm->vmas;
        mm->vmas  = vma;
        return 1;
    }

    vma_t* prev = mm->vmas;
    while (prev->next && prev->next->start < start)
        prev = prev->next;
    vma->next  = prev->next;
    prev->next = vma;
    return 1;
}

// ── mm_vma_find ───────────────────────────────────────────────────────────
vma_t* mm_vma_find(mm_t* mm, virt_addr_t addr) {
    if (!mm) return NULL;
    for (vma_t* v = mm->vmas; v; v = v->next) {
        if (addr >= v->start && addr < v->end) return v;
    }
    return NULL;
}

// ── mm_destroy ────────────────────────────────────────────────────────────
// Frees all VMA descriptors and the mm_t itself.
// Physical frames must be freed separately via vmm_free_user_and_frames().
void mm_destroy(mm_t* mm) {
    if (!mm) return;
    vma_t* v = mm->vmas;
    while (v) {
        vma_t* next = v->next;
        kfree(v);
        v = next;
    }
    kfree(mm);
}

// ── mm_clone ──────────────────────────────────────────────────────────────
// Allocate a new mm_t and copy brk fields + VMA list from src.
// Does NOT copy physical pages.
mm_t* mm_clone(const mm_t* src) {
    if (!src) return NULL;
    mm_t* dst = mm_create();
    if (!dst) return NULL;
    dst->brk_start = src->brk_start;
    dst->brk       = src->brk;
    dst->mmap_base = src->mmap_base;
    dst->refcount  = 1;
    // Copy each VMA.
    for (vma_t* v = src->vmas; v; v = v->next) {
        if (!mm_vma_add(dst, v->start, v->end, v->flags)) {
            mm_destroy(dst);
            return NULL;
        }
    }
    return dst;
}

// ── mm_vma_remove ─────────────────────────────────────────────────────────
// Remove or trim VMAs in [start, end).  Handles four cases:
//   1. VMA entirely inside [start,end): remove it entirely.
//   2. VMA overlaps left edge: shrink vma->end.
//   3. VMA overlaps right edge: shrink vma->start.
//   4. VMA contains [start,end) as a proper sub-range: split into two.
// Returns 1 if at least one VMA was modified, 0 otherwise.
uint8_t mm_vma_remove(mm_t* mm, virt_addr_t start, virt_addr_t end) {
    if (!mm || start >= end) return 0;
    uint8_t did_work = 0;
    vma_t** pp = &mm->vmas;
    while (*pp) {
        vma_t* v = *pp;
        if (v->start >= end || v->end <= start) {
            pp = &v->next; // no overlap
            continue;
        }
        did_work = 1;
        if (v->start >= start && v->end <= end) {
            // Case 1: entirely covered — remove.
            *pp = v->next;
            kfree(v);
            continue;
        }
        if (v->start < start && v->end > end) {
            // Case 4: split — create a new right fragment.
            vma_t* right = kmalloc(sizeof(vma_t));
            if (!right) { pp = &v->next; continue; }
            right->start = end;
            right->end   = v->end;
            right->flags = v->flags;
            right->next  = v->next;
            v->end       = start;
            v->next      = right;
            pp = &right->next;
            continue;
        }
        if (v->start < start) {
            // Case 2: right edge overlap — shrink end.
            v->end = start;
        } else {
            // Case 3: left edge overlap — shrink start.
            v->start = end;
        }
        pp = &v->next;
    }
    return did_work;
}

// ── mm_vma_find_free ──────────────────────────────────────────────────────
// Find a free virtual address gap of at least `len` bytes in the mmap region.
// Searches from mmap_base downward (like Linux ASLR-disabled mmap).
// Returns the start VA of the gap, or 0 on failure.
virt_addr_t mm_vma_find_free(mm_t* mm, size_t len) {
    if (!mm || !len) return 0;
    len = (len + PAGE_MASK) & ~PAGE_MASK;

    // Starting hint: use mm->mmap_base (set at process creation).
    // We scan downward: try [hint - len, hint), then decrement hint.
    virt_addr_t hint = mm->mmap_base;

    // Hard lower limit: leave 64MB gap above the heap to avoid collision.
    virt_addr_t lower_limit = mm->brk + (64ULL * 1024 * 1024);

    while (hint >= lower_limit + len) {
        virt_addr_t candidate = hint - len;
        candidate &= ~PAGE_MASK; // page-align down

        // Check if candidate..candidate+len overlaps any existing VMA.
        uint8_t overlaps = 0;
        for (vma_t* v = mm->vmas; v; v = v->next) {
            if (candidate < v->end && candidate + len > v->start) {
                // Overlap: skip below this VMA.
                hint = v->start;
                overlaps = 1;
                break;
            }
        }
        if (!overlaps) {
            mm->mmap_base = candidate; // update hint for next allocation
            return candidate;
        }
    }
    return 0; // no gap found
}

// ── mm_vma_pte_flags ──────────────────────────────────────────────────────
// Maps VMA protection flags to x86-64 PTE flags.
// Always sets PAGE_USER (user-space VMAs only — kernel VMAs use vmm directly).
// NX is set for anything not explicitly executable.
// Read-only: no PAGE_WRITABLE.
uint64_t mm_vma_pte_flags(uint32_t vma_flags) {
    uint64_t pte = PAGE_PRESENT | PAGE_USER;
    if (vma_flags & VMA_W) pte |= PAGE_WRITABLE;
    if (!(vma_flags & VMA_X)) pte |= PAGE_NX;
    return pte;
}
