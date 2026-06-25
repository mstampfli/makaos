#include "mm.h"
#include "shmem.h"
#include "kheap.h"
#include "vmm.h"
#include "common.h"
#include "rcu.h"
#include "vfs.h"

/* ── C library stubs required by the compiler ────────────────────────────
 *
 * GCC with -fno-builtin still emits calls to memcpy/memmove/memset for
 * large __builtin_memcpy/memset calls it can't inline.  The kernel has no
 * libc, so we must provide these — and they absolutely cannot be byte
 * loops (fb_term_scroll copies ~8 MB every time the console scrolls).
 *
 * Use `rep movsb`/`rep stosb` — on Intel Ivy Bridge and later these are
 * as fast as SSE/AVX copies thanks to ERMS (Enhanced REP MOVSB), and
 * crucially they don't touch SSE state (the kernel is compiled with
 * -mno-sse and doesn't save/restore XMM registers on context switch).
 */
void* memcpy(void* dst, const void* src, size_t n) {
    __asm__ volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n)
                      :
                      : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        // Forward copy: safe since dst doesn't overlap with remaining src.
        __asm__ volatile ("rep movsb"
                          : "+D"(d), "+S"(s), "+c"(n)
                          :
                          : "memory");
    } else {
        // Backward copy: set DF=1, start from the end, rep movsb, clear DF.
        d += n - 1;
        s += n - 1;
        __asm__ volatile ("std\n\t"
                          "rep movsb\n\t"
                          "cld"
                          : "+D"(d), "+S"(s), "+c"(n)
                          :
                          : "memory");
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    __asm__ volatile ("rep stosb"
                      : "+D"(dst), "+c"(n)
                      : "a"((uint8_t)c)
                      : "memory");
    return ret;
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
    spin_lock_init(&mm->vma_lock);
    return mm;
}

// call_rcu callback: free a VMA after its grace period.  Must drop the
// shmem ref here too so the shmem_t lifetime tracks the VMA's actual
// unreachability, not the moment we unlinked it.
void vma_free_rcu(void* data) {
    vma_t* v = (vma_t*)data;
    if (v->shmem) shmem_unref(v->shmem);
    if (v->file)  vfs_close(v->file);
    kfree(v);
}

// ── mm_vma_add ────────────────────────────────────────────────────────────
// Inserts a new VMA into the sorted list.
// Security: rejects misaligned addresses, rejects zero-size or overlapping
// regions.  Writer-side: serialised on mm->vma_lock.  Publishes via
// rcu_assign_pointer so readers walking under rcu_read_lock either see the
// old list or the new node atomically.
uint8_t mm_vma_add(mm_t* mm, virt_addr_t start, virt_addr_t end, uint32_t flags) {
    if (!mm) return 0;
    if (start >= end)              return 0;
    if (start & PAGE_MASK)         return 0;
    if (end   & PAGE_MASK)         return 0;

    vma_t* vma = kmalloc(sizeof(vma_t));
    if (!vma) return 0;
    vma->start       = start;
    vma->end         = end;
    vma->flags       = flags;
    vma->next        = NULL;
    vma->shmem       = NULL;
    vma->shmem_pgoff = 0;
    vma->file        = NULL;
    vma->file_off    = 0;
    vma->file_len    = 0;

    spin_lock(&mm->vma_lock);

    // Overlap check under the writer lock.
    for (vma_t* v = mm->vmas; v; v = v->next) {
        if (start < v->end && end > v->start) {
            spin_unlock(&mm->vma_lock);
            kfree(vma);
            return 0;
        }
    }

    // Insert sorted by start address.  Every ->next store that becomes
    // visible to readers goes through rcu_assign_pointer so a concurrent
    // walker cannot observe a half-built link.
    if (!mm->vmas || start < mm->vmas->start) {
        vma->next = mm->vmas;                   // no readers see this yet
        rcu_assign_pointer(mm->vmas, vma);
    } else {
        vma_t* prev = mm->vmas;
        while (prev->next && prev->next->start < start) prev = prev->next;
        vma->next = prev->next;                 // private initialisation
        rcu_assign_pointer(prev->next, vma);
    }
    spin_unlock(&mm->vma_lock);
    return 1;
}

// ── mm_vma_add_file ───────────────────────────────────────────────────────
// Insert a fully-initialised file-backed VMA in one pass.
// All fields — including file, file_off, file_len — are set before the VMA
// is published via rcu_assign_pointer, so no reader ever sees VMA_FILE with
// a NULL file pointer.  No second lock acquisition needed.
uint8_t mm_vma_add_file(mm_t* mm, virt_addr_t start, virt_addr_t end,
                          uint32_t prot_flags,
                          struct vfs_file_t* file,
                          uint64_t file_off, uint64_t file_len) {
    if (!mm || !file)      return 0;
    if (start >= end)      return 0;
    if (start & PAGE_MASK) return 0;
    if (end   & PAGE_MASK) return 0;

    vma_t* vma = kmalloc(sizeof(vma_t));
    if (!vma) return 0;

    // Fully initialise before insertion — VMA is private until rcu_assign_pointer.
    vma->start       = start;
    vma->end         = end;
    vma->flags       = VMA_FILE | prot_flags;
    vma->next        = NULL;
    vma->shmem       = NULL;
    vma->shmem_pgoff = 0;
    vma->file        = vfs_dup(file);
    vma->file_off    = file_off;
    vma->file_len    = file_len;

    spin_lock(&mm->vma_lock);

    for (vma_t* v = mm->vmas; v; v = v->next) {
        if (start < v->end && end > v->start) {
            spin_unlock(&mm->vma_lock);
            vfs_close(vma->file);
            kfree(vma);
            return 0;
        }
    }

    if (!mm->vmas || start < mm->vmas->start) {
        vma->next = mm->vmas;
        rcu_assign_pointer(mm->vmas, vma);
    } else {
        vma_t* prev = mm->vmas;
        while (prev->next && prev->next->start < start) prev = prev->next;
        vma->next = prev->next;
        rcu_assign_pointer(prev->next, vma);
    }
    spin_unlock(&mm->vma_lock);
    return 1;
}

// ── mm_vma_find ───────────────────────────────────────────────────────────
// RCU reader walk.  Caller must hold rcu_read_lock() for the lifetime of
// the returned pointer.  On x86 rcu_dereference is a plain load plus a
// compiler barrier — the whole walk is three instructions per node.
vma_t* mm_vma_find(mm_t* mm, virt_addr_t addr) {
    if (!mm) return NULL;
    for (vma_t* v = rcu_dereference(mm->vmas); v;
                 v = rcu_dereference(v->next)) {
        if (addr >= v->start && addr < v->end) return v;
    }
    return NULL;
}

// Every page in [addr, addr+len) backed by a VMA?  See the header comment: this
// is the no-map, file-safe alternative to a prefault for validating a user
// buffer the kernel will raw-deref for READING.  It does NOT close the TOCTOU
// vs a concurrent munmap between this check and the caller's raw read -- that
// residual needs a fault-fixup table (recorded follow-up).
int mm_range_has_vmas(mm_t* mm, uint64_t addr, uint64_t len) {
    if (!len) return 1;                          // empty range is trivially ok
    if (!mm)  return 0;
    uint64_t addr_end = addr + len;
    if (addr_end < addr) return 0;               // addr+len wraps u64
    virt_addr_t page   = addr & ~(virt_addr_t)0xFFF;
    uint64_t    npages = ((addr_end - 1) >> 12) - (addr >> 12) + 1;
    for (uint64_t i = 0; i < npages; i++, page += 0x1000) {
        rcu_read_lock();
        int have = (mm_vma_find(mm, page) != NULL);
        rcu_read_unlock();
        if (!have) return 0;                     // a page with no VMA -> -EFAULT
    }
    return 1;
}

// Deterministic selftest for mm_range_has_vmas (the F67 fix core): two VMAs with
// a gap, exercising fully-inside, straddle-into-gap, in-gap, mid-page, len==0,
// NULL mm, and the addr+len wrap.  Pure -- a constructed mm, no real faults.
void mm_range_has_vmas_selftest(void) {
    extern void kprintf_atomic(const char*, ...);
    int fails = 0;
    vma_t v2 = { .start = 0x20000, .end = 0x21000, .next = NULL };
    vma_t v1 = { .start = 0x10000, .end = 0x12000, .next = &v2 };
    mm_t m; __builtin_memset(&m, 0, sizeof(m)); m.vmas = &v1;

    struct { uint64_t a, l; int want; const char* d; } cs[] = {
        { 0x10000, 0x1000, 1, "page fully in v1" },
        { 0x10000, 0x2000, 1, "both pages of v1" },
        { 0x10800, 0x800,  1, "mid-page within v1" },
        { 0x11FFF, 0x2,    0, "straddles v1 end into gap" },
        { 0x12000, 0x8,    0, "in the gap (no VMA)" },
        { 0x1F000, 0x8,    0, "in the gap before v2" },
        { 0x20000, 0x1000, 1, "page fully in v2" },
        { 0x10000, 0,      1, "len 0 trivially ok" },
        { 0xFFFFFFFFFFFFF000ULL, 0x2000, 0, "addr+len wraps" },
    };
    for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        int got = mm_range_has_vmas(&m, cs[i].a, cs[i].l);
        if (got != cs[i].want) { fails++;
            kprintf_atomic("[mm_range_vmas] FAIL %s: got %d want %d\n",
                           cs[i].d, got, cs[i].want); }
    }
    if (mm_range_has_vmas(NULL, 0x10000, 0x1000) != 0) { fails++;
        kprintf_atomic("[mm_range_vmas] FAIL null mm\n"); }

    kprintf_atomic(fails ? "[mm_range_vmas] SELF-TEST FAILED\n"
                         : "[mm_range_vmas] PASS (per-page VMA backing for raw-deref user reads)\n");
}

// ── mm_destroy ────────────────────────────────────────────────────────────
// Frees all VMA descriptors and the mm_t itself.
// Called at last task_mm_t unref — by that point there are no threads
// still using this address space and no readers can race us.  Safe to
// free synchronously (no call_rcu needed).
void mm_destroy(mm_t* mm) {
    if (!mm) return;
    vma_t* v = mm->vmas;
    while (v) {
        vma_t* next = v->next;
        if (v->shmem) shmem_unref(v->shmem);
        if (v->file)  vfs_close(v->file);
        kfree(v);
        v = next;
    }
    kfree(mm);
}

// ── mm_clone ──────────────────────────────────────────────────────────────
// Allocate a new mm_t and copy brk fields + VMA list from src.
// Does NOT copy physical pages.  Walk src under rcu_read_lock so a
// concurrent sibling mutating src (threaded parent) cannot free a VMA
// under our feet.  We copy out v->start/end/flags/shmem before calling
// mm_vma_add (which drops the lock via kmalloc), so the reader section
// stays tight.
mm_t* mm_clone(const mm_t* src) {
    if (!src) return NULL;
    mm_t* dst = mm_create();
    if (!dst) return NULL;
    dst->brk_start = src->brk_start;
    dst->brk       = src->brk;
    dst->mmap_base = src->mmap_base;
    dst->refcount  = 1;

    // First pass: snapshot the src list into a stack array so we can drop
    // the RCU reader before calling mm_vma_add (which allocates and takes
    // dst's writer lock).
    typedef struct {
        virt_addr_t        start, end;
        uint32_t           flags;
        struct shmem*      shmem;
        uint32_t           shmem_pgoff;
        struct vfs_file_t* file;
        uint64_t           file_off;
        uint64_t           file_len;
    } snap_t;
    // Bound: count entries first.
    uint32_t n = 0;
    rcu_read_lock();
    for (vma_t* v = rcu_dereference(src->vmas); v;
                 v = rcu_dereference(v->next)) n++;
    rcu_read_unlock();
    if (n == 0) return dst;

    snap_t* snap = kmalloc(n * sizeof(snap_t));
    if (!snap) { mm_destroy(dst); return NULL; }

    rcu_read_lock();
    uint32_t i = 0;
    for (vma_t* v = rcu_dereference(src->vmas); v && i < n;
                 v = rcu_dereference(v->next), i++) {
        snap[i].start       = v->start;
        snap[i].end         = v->end;
        snap[i].flags       = v->flags;
        snap[i].shmem       = v->shmem;
        snap[i].shmem_pgoff = v->shmem_pgoff;
        snap[i].file        = v->file;
        snap[i].file_off    = v->file_off;
        snap[i].file_len    = v->file_len;
    }
    rcu_read_unlock();
    uint32_t cnt = i;

    for (uint32_t k = 0; k < cnt; k++) {
        if (!mm_vma_add(dst, snap[k].start, snap[k].end, snap[k].flags)) {
            kfree(snap);
            mm_destroy(dst);
            return NULL;
        }
        if (snap[k].shmem || snap[k].file) {
            rcu_read_lock();
            vma_t* dv = mm_vma_find(dst, snap[k].start);
            if (dv) {
                if (snap[k].shmem) {
                    dv->shmem       = snap[k].shmem;
                    dv->shmem_pgoff = snap[k].shmem_pgoff;
                    shmem_ref(snap[k].shmem);
                }
                if (snap[k].file) {
                    dv->file     = vfs_dup(snap[k].file);
                    dv->file_off = snap[k].file_off;
                    dv->file_len = snap[k].file_len;
                }
            }
            rcu_read_unlock();
        }
    }
    kfree(snap);
    return dst;
}

// ── mm_vma_remove ─────────────────────────────────────────────────────────
// Remove or trim VMAs in [start, end).  Handles four cases:
//   1. VMA entirely inside [start,end): remove it entirely.
//   2. VMA overlaps left edge: shrink vma->end.
//   3. VMA overlaps right edge: shrink vma->start.
//   4. VMA contains [start,end) as a proper sub-range: split into two.
//
// RCU concurrency: trimming in place (cases 2/3) is done as a plain
// field update under the writer lock — readers walk bounds via
// `addr >= v->start && addr < v->end`, so the worst case for a
// racing reader is observing a transitional bound and either
// matching or not matching; both outcomes are safe because the page
// was going to be unmapped from the page tables anyway by the caller.
// Case 1 (remove) unlinks via rcu_assign_pointer and defers the free
// via call_rcu.  Case 4 (split) builds the right fragment, links it
// in under the writer lock, then trims the left in place.
uint8_t mm_vma_remove(mm_t* mm, virt_addr_t start, virt_addr_t end) {
    if (!mm || start >= end) return 0;
    uint8_t did_work = 0;
    spin_lock(&mm->vma_lock);
    vma_t** pp = &mm->vmas;
    while (*pp) {
        vma_t* v = *pp;
        if (v->start >= end || v->end <= start) {
            pp = &v->next; // no overlap
            continue;
        }
        did_work = 1;
        if (v->start >= start && v->end <= end) {
            // Case 1: entirely covered — unlink via rcu_assign_pointer,
            // then defer the free.  Expedited: mm_vma_remove is called
            // from munmap / MAP_FIXED replace, user-syscall return path.
            rcu_assign_pointer(*pp, v->next);
            call_rcu_head(&v->rcu_head, vma_free_rcu, v);
            continue;
        }
        if (v->start < start && v->end > end) {
            // Case 4: split.  Build the right fragment in private memory
            // first, then publish by linking it after v and trimming v.
            vma_t* right = kmalloc(sizeof(vma_t));
            if (!right) { pp = &v->next; continue; }
            right->start       = end;
            right->end         = v->end;
            right->flags       = v->flags;
            right->next        = v->next;   // private init
            right->shmem       = v->shmem;
            right->shmem_pgoff = v->shmem_pgoff +
                (uint32_t)((end - v->start) / PAGE_SIZE);
            if (right->shmem) shmem_ref(right->shmem);
            // File backing: kmalloc memory is NOT zeroed — leaving these
            // uninitialized gave the right fragment a GARBAGE vfs_file_t
            // pointer; the next fault on it did disk I/O through wild
            // function pointers.  Carry the backing over with its own
            // reference and a file offset advanced by the split.
            if (v->file) {
                right->file     = vfs_dup(v->file);
                right->file_off = v->file_off + (end - v->start);
                right->file_len = (v->file_len > (end - v->start))
                                  ? v->file_len - (end - v->start) : 0;
            } else {
                right->file     = NULL;
                right->file_off = 0;
                right->file_len = 0;
            }
            // Publish right, then shrink v.  A concurrent reader sees
            // either {v covers [v->start, old_end)} or
            // {v covers [v->start, start), right covers [end, old_end)} —
            // the gap [start, end) is never mapped in page tables while
            // a walker could observe it.
            rcu_assign_pointer(v->next, right);
            v->end = start;
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
    spin_unlock(&mm->vma_lock);
    return did_work;
}

// ── mm_vma_find_free ──────────────────────────────────────────────────────
// Find a free virtual address gap of at least `len` bytes in the mmap
// region.  Writer-side: serialises on mm->vma_lock against concurrent
// mmap/munmap/brk so the gap computed here is still valid when the
// caller inserts its new VMA.  (The caller typically holds vma_lock
// internally via mm_vma_add right after.)
virt_addr_t mm_vma_find_free(mm_t* mm, size_t len) {
    if (!mm || !len) return 0;
    len = (len + PAGE_MASK) & ~PAGE_MASK;

    spin_lock(&mm->vma_lock);
    virt_addr_t hint = mm->mmap_base;
    virt_addr_t lower_limit = mm->brk + (64ULL * 1024 * 1024);

    while (hint >= lower_limit + len) {
        virt_addr_t candidate = hint - len;
        candidate &= ~PAGE_MASK;

        uint8_t overlaps = 0;
        for (vma_t* v = mm->vmas; v; v = v->next) {
            if (candidate < v->end && candidate + len > v->start) {
                hint = v->start;
                overlaps = 1;
                break;
            }
        }
        if (!overlaps) {
            mm->mmap_base = candidate;
            spin_unlock(&mm->vma_lock);
            return candidate;
        }
    }
    spin_unlock(&mm->vma_lock);
    return 0;
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
