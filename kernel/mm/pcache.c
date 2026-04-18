#include "pcache.h"
#include "pmm.h"
#include "smp.h"
#include "kheap.h"
#include "process.h"
#include "sched.h"
#include "wait.h"

// ── Hash table layout ─────────────────────────────────────────────────────
// Power-of-2 bucket count for fast masking.

#define PCACHE_BUCKETS  1024u
#define PCACHE_MASK     (PCACHE_BUCKETS - 1u)

// ── Reclaim policy ────────────────────────────────────────────────────────
// The page cache targets 80% of free memory.  A background kthread
// (pcache_reclaim_thread) maintains this ratio proactively — it wakes
// when the cache exceeds its target and evicts CLOCK victims until the
// target is met.  Allocators never block on reclaim.
//
// "Free memory" = pmm_free_pages + pcache_count (reclaimable pages).
// The cache target = 80% of that pool.  The remaining 20% stays as a
// genuine free-page buffer for instant allocation.

#define PCACHE_TARGET_PCT  80u   // cache may use up to 80% of free pool
#define PCACHE_BUFFER_PCT  20u   // 20% stays free

typedef struct pcache_entry {
    uint32_t             ino;
    uint32_t             pg_idx;
    phys_addr_t          frame;
    struct pcache_entry* next;       // hash chain
    struct pcache_entry* clock_next; // CLOCK ring (global)
    struct pcache_entry* clock_prev;
    uint8_t              accessed;   // CLOCK bit — set on lookup, cleared by hand
    uint8_t              bucket_idx_lo; // low 8 bits of bucket (for fast evict)
} pcache_entry_t;

typedef struct {
    spinlock_t       lock;
    pcache_entry_t*  head;
} pcache_bucket_t;

static pcache_bucket_t g_buckets[PCACHE_BUCKETS];

// ── CLOCK ring ────────────────────────────────────────────────────────────
// Doubly-linked circular list of all cached entries.  The CLOCK hand
// rotates through looking for entries with accessed==0 to evict.
// Protected by g_clock_lock (separate from bucket locks to avoid nesting).

static spinlock_t      g_clock_lock = SPINLOCK_INIT;
static pcache_entry_t* g_clock_hand = NULL;
static volatile uint64_t g_pcache_count = 0;  // total cached pages

static uint8_t g_ready = 0;

// ── Reclaim kthread state ─────────────────────────────────────────────────
static wait_queue_t g_reclaim_wq;
static volatile uint8_t g_reclaim_needed = 0;

// ── CLOCK ring helpers (caller holds g_clock_lock) ────────────────────────

static void clock_insert(pcache_entry_t* e) {
    if (!g_clock_hand) {
        e->clock_next = e;
        e->clock_prev = e;
        g_clock_hand  = e;
    } else {
        e->clock_next = g_clock_hand;
        e->clock_prev = g_clock_hand->clock_prev;
        g_clock_hand->clock_prev->clock_next = e;
        g_clock_hand->clock_prev = e;
    }
    __atomic_fetch_add(&g_pcache_count, 1, __ATOMIC_RELAXED);
}

static void clock_remove(pcache_entry_t* e) {
    if (e->clock_next == e) {
        // Last entry in ring.
        g_clock_hand = NULL;
    } else {
        e->clock_prev->clock_next = e->clock_next;
        e->clock_next->clock_prev = e->clock_prev;
        if (g_clock_hand == e)
            g_clock_hand = e->clock_next;
    }
    e->clock_next = NULL;
    e->clock_prev = NULL;
    __atomic_fetch_sub(&g_pcache_count, 1, __ATOMIC_RELAXED);
}

// ── Init ──────────────────────────────────────────────────────────────────

void pcache_init(void) {
    for (uint32_t i = 0; i < PCACHE_BUCKETS; i++) {
        spin_lock_init(&g_buckets[i].lock);
        g_buckets[i].head = NULL;
    }
    wait_queue_init(&g_reclaim_wq);
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

// ── Hash ──────────────────────────────────────────────────────────────────

static inline uint32_t pcache_hash(uint32_t ino, uint32_t pg_idx) {
    uint64_t k = ((uint64_t)ino << 32) | (uint64_t)pg_idx;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)(k & PCACHE_MASK);
}

// ── Reclaim trigger ───────────────────────────────────────────────────────
// Called after insert to check if cache exceeds its target.
// Wakes the reclaim kthread if so — never blocks.

static void pcache_maybe_wake_reclaim(void) {
    uint64_t cached = __atomic_load_n(&g_pcache_count, __ATOMIC_RELAXED);
    uint64_t free   = pmm_free_pages_get();
    uint64_t pool   = free + cached;  // total reclaimable + free

    // Target: cached should be at most (pool * 80 / 100).
    uint64_t target = pool * PCACHE_TARGET_PCT / 100u;
    if (cached > target) {
        __atomic_store_n(&g_reclaim_needed, 1, __ATOMIC_RELEASE);
        wait_queue_wake_all(&g_reclaim_wq);
    }
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
            e->accessed = 1;  // CLOCK: mark as recently used
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

    pcache_entry_t* ne = (pcache_entry_t*)kmalloc(sizeof(pcache_entry_t));
    if (!ne)
        return PMM_INVALID_ADDR;

    uint32_t h = pcache_hash(ino, pg_idx);

    ne->ino        = ino;
    ne->pg_idx     = pg_idx;
    ne->frame      = frame;
    ne->next       = NULL;
    ne->clock_next = NULL;
    ne->clock_prev = NULL;
    ne->accessed   = 1;
    ne->bucket_idx_lo = (uint8_t)(h & 0xFFu);

    pcache_bucket_t* b = &g_buckets[h];

    spin_lock(&b->lock);

    // Race check: another CPU may have inserted the same (ino, pg_idx).
    pcache_entry_t* e = b->head;
    while (e) {
        if (e->ino == ino && e->pg_idx == pg_idx) {
            phys_addr_t existing = e->frame;
            spin_unlock(&b->lock);
            kfree(ne);
            pmm_ref_dec(frame);
            return existing;
        }
        e = e->next;
    }

    // Insert into hash chain.
    ne->next = b->head;
    b->head  = ne;
    spin_unlock(&b->lock);

    // Insert into CLOCK ring.
    spin_lock(&g_clock_lock);
    clock_insert(ne);
    spin_unlock(&g_clock_lock);

    pcache_maybe_wake_reclaim();
    return frame;
}

void pcache_evict_inode(uint32_t ino) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || ino == 0)
        return;

    for (uint32_t h = 0; h < PCACHE_BUCKETS; h++) {
        pcache_bucket_t* b = &g_buckets[h];
        if (!b->head) continue;

        spin_lock(&b->lock);
        pcache_entry_t** pp = &b->head;
        while (*pp) {
            pcache_entry_t* e = *pp;
            if (e->ino == ino) {
                *pp = e->next;

                // Remove from CLOCK ring.
                spin_lock(&g_clock_lock);
                clock_remove(e);
                spin_unlock(&g_clock_lock);

                pmm_ref_dec(e->frame);
                kfree(e);
            } else {
                pp = &e->next;
            }
        }
        spin_unlock(&b->lock);
    }
}

// ── CLOCK eviction ────────────────────────────────────────────────────────
// Evict one page using the CLOCK algorithm.
// Returns 1 if a page was evicted, 0 if the cache is empty or all pages
// are pinned (refcount > 1, meaning a PTE still maps them — we can't
// evict those).

static uint8_t pcache_evict_one(void) {
    spin_lock(&g_clock_lock);
    if (!g_clock_hand) {
        spin_unlock(&g_clock_lock);
        return 0;
    }

    // Scan at most 2× the cache size (one pass to clear accessed bits,
    // one to find a victim).  Prevents infinite loop if all entries are
    // accessed.
    uint64_t limit = __atomic_load_n(&g_pcache_count, __ATOMIC_RELAXED) * 2;
    if (limit < 2) limit = 2;

    for (uint64_t i = 0; i < limit; i++) {
        pcache_entry_t* cand = g_clock_hand;
        g_clock_hand = cand->clock_next;

        if (cand->accessed) {
            cand->accessed = 0;
            continue;
        }

        // Check if the frame is still mapped by any PTE (refcount > 1).
        // If so, skip — we can't free a frame that's in use.
        uint32_t rc = pmm_ref_get(cand->frame);
        if (rc > 1) {
            // Still mapped.  Mark accessed so we don't spin on it.
            cand->accessed = 1;
            continue;
        }

        // Victim found.  Remove from CLOCK ring first, then from hash.
        phys_addr_t victim_frame = cand->frame;
        uint32_t victim_ino      = cand->ino;
        uint32_t victim_pg_idx   = cand->pg_idx;

        clock_remove(cand);
        spin_unlock(&g_clock_lock);

        // Remove from hash bucket.
        uint32_t h = pcache_hash(victim_ino, victim_pg_idx);
        pcache_bucket_t* b = &g_buckets[h];

        spin_lock(&b->lock);
        pcache_entry_t** pp = &b->head;
        while (*pp) {
            if (*pp == cand) {
                *pp = cand->next;
                break;
            }
            pp = &(*pp)->next;
        }
        spin_unlock(&b->lock);

        pmm_ref_dec(victim_frame);
        kfree(cand);
        return 1;
    }

    spin_unlock(&g_clock_lock);
    return 0;  // no evictable victim found
}

// ── Reclaim kthread ───────────────────────────────────────────────────────
// Sleeps until woken by pcache_maybe_wake_reclaim().  Evicts CLOCK victims
// until the cache is back within its target (80% of free pool).

static void pcache_reclaim_thread(void) {
    for (;;) {
        // Sleep until signalled.
        task_we_t we;
        task_we_init(&we, g_current);
        task_we_add(&g_reclaim_wq, &we);

        if (!__atomic_load_n(&g_reclaim_needed, __ATOMIC_ACQUIRE)) {
            sched_sleep();
        }
        task_we_remove(&g_reclaim_wq, &we);
        __atomic_store_n(&g_reclaim_needed, 0, __ATOMIC_RELEASE);

        // Evict until we're back within target.
        for (;;) {
            uint64_t cached = __atomic_load_n(&g_pcache_count, __ATOMIC_RELAXED);
            uint64_t free   = pmm_free_pages_get();
            uint64_t pool   = free + cached;
            uint64_t target = pool * PCACHE_TARGET_PCT / 100u;

            if (cached <= target || cached == 0)
                break;

            if (!pcache_evict_one())
                break;  // nothing evictable
        }
    }
}

uint64_t pcache_count_get(void) {
    return __atomic_load_n(&g_pcache_count, __ATOMIC_RELAXED);
}

void pcache_start_reclaim_thread(void) {
    task_t* t = task_create_kthread(pcache_reclaim_thread, pid_alloc());
    if (t) sched_add(t);
}
