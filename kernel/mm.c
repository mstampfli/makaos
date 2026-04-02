#include "mm.h"
#include "kheap.h"
#include "vmm.h"

// ── mm_create ─────────────────────────────────────────────────────────────
mm_t* mm_create(void) {
    mm_t* mm = kmalloc(sizeof(mm_t));
    if (!mm) return NULL;
    mm->vmas      = NULL;
    mm->brk_start = 0;
    mm->brk       = 0;
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

    // W^X: refuse writable+executable mappings.
    if ((flags & VMA_W) && (flags & VMA_X)) return 0;

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
