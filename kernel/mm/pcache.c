#include "pcache.h"
#include "pmm.h"
#include "smp.h"
#include "kheap.h"  // kmalloc / kfree

// ── Hash table layout ─────────────────────────────────────────────────────
// Power-of-2 bucket count for fast masking.
// 1024 buckets × 16 bytes/bucket (lock + head ptr) = 16 KiB static data.
// With typical workloads (tens of binaries, hundreds of pages each), average
// bucket depth stays under 1 — O(1) average lookup.

#define PCACHE_BUCKETS  1024u
#define PCACHE_MASK     (PCACHE_BUCKETS - 1u)

typedef struct pcache_entry {
    uint32_t            ino;
    uint32_t            pg_idx;
    phys_addr_t         frame;
    struct pcache_entry* next;
} pcache_entry_t;

typedef struct {
    spinlock_t       lock;
    pcache_entry_t*  head;
} pcache_bucket_t;

static pcache_bucket_t g_buckets[PCACHE_BUCKETS];
static uint8_t g_ready = 0;

void pcache_init(void) {
    for (uint32_t i = 0; i < PCACHE_BUCKETS; i++) {
        spin_lock_init(&g_buckets[i].lock);
        g_buckets[i].head = NULL;
    }
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

// ── Internal helpers ──────────────────────────────────────────────────────

// Mix (ino, pg_idx) into a uniform 32-bit bucket index.
// Using a multiply-based hash to distribute ELF page patterns (pg_idx is
// often small and sequential; ino is small and dense — a plain XOR or
// modulo would cluster into the first few buckets).
static inline uint32_t pcache_hash(uint32_t ino, uint32_t pg_idx) {
    uint64_t k = ((uint64_t)ino << 32) | (uint64_t)pg_idx;
    // Murmur-inspired finaliser
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)(k & PCACHE_MASK);
}

// ── Public API ────────────────────────────────────────────────────────────

phys_addr_t pcache_get(uint32_t ino, uint32_t pg_idx) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || ino == 0)
        return PMM_INVALID_ADDR;

    uint32_t h = pcache_hash(ino, pg_idx);
    pcache_bucket_t* b = &g_buckets[h];

    spin_lock(&b->lock);
    pcache_entry_t* e = b->head;
    while (e) {
        if (e->ino == ino && e->pg_idx == pg_idx) {
            phys_addr_t frame = e->frame;
            spin_unlock(&b->lock);
            return frame;
        }
        e = e->next;
    }
    spin_unlock(&b->lock);
    return PMM_INVALID_ADDR;
}

phys_addr_t pcache_insert(uint32_t ino, uint32_t pg_idx, phys_addr_t frame) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || ino == 0)
        return PMM_INVALID_ADDR;

    // Allocate the entry node BEFORE taking the lock so we hold the lock
    // as briefly as possible.
    pcache_entry_t* ne = (pcache_entry_t*)kmalloc(sizeof(pcache_entry_t));
    if (!ne) {
        // OOM: can't cache.  Caller keeps `frame` (rc==1) as their PTE ref.
        return PMM_INVALID_ADDR;
    }
    ne->ino    = ino;
    ne->pg_idx = pg_idx;
    ne->frame  = frame;
    ne->next   = NULL;

    uint32_t h = pcache_hash(ino, pg_idx);
    pcache_bucket_t* b = &g_buckets[h];

    spin_lock(&b->lock);

    // Race check: another CPU may have inserted while we were reading from disk.
    pcache_entry_t* e = b->head;
    while (e) {
        if (e->ino == ino && e->pg_idx == pg_idx) {
            // Already present.  Drop our duplicate.
            phys_addr_t existing = e->frame;
            spin_unlock(&b->lock);
            kfree(ne);
            pmm_ref_dec(frame);   // free the alloc ref we were given
            return existing;      // caller: pmm_ref_inc(existing) for their PTE
        }
        e = e->next;
    }

    // Insert at head (LIFO — most recently used will be found first if
    // the same page is re-faulted, though in practice each (ino,pg_idx)
    // is inserted exactly once).
    ne->next = b->head;
    b->head  = ne;

    spin_unlock(&b->lock);
    // Cache now owns the alloc ref.  Caller: pmm_ref_inc(frame) for their PTE.
    return frame;
}

void pcache_evict_inode(uint32_t ino) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || ino == 0)
        return;

    // Walk all buckets — necessary since any bucket may contain entries for
    // this ino due to the hash.  This is an O(buckets + entries_for_ino)
    // operation but is only called on file write or unlink, not in hot paths.
    for (uint32_t h = 0; h < PCACHE_BUCKETS; h++) {
        pcache_bucket_t* b = &g_buckets[h];

        // Quick check without lock first (benign race — we recheck under lock).
        if (!b->head) continue;

        spin_lock(&b->lock);
        pcache_entry_t** pp = &b->head;
        while (*pp) {
            pcache_entry_t* e = *pp;
            if (e->ino == ino) {
                *pp = e->next;
                pmm_ref_dec(e->frame);
                kfree(e);
            } else {
                pp = &e->next;
            }
        }
        spin_unlock(&b->lock);
    }
}
