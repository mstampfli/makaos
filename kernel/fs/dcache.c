// ── VFS directory-entry cache (Phase 7A) ──────────────────────────────
// See dcache.h for design & invariants.  Readers take rcu_read_lock,
// walk a hash bucket lockless via rcu_dereference, and bump the
// hit's refcount with a single atomic before releasing rcu.  Writers
// take g_dcache_wlock for list mutation and hand old nodes to
// call_rcu_head.

#include "dcache.h"
#include "common.h"
#include "rcu.h"
#include "smp.h"
#include "pmm.h"
#include "kheap.h"
#include "preempt.h"

extern uint64_t tsc_read_ns(void);

// ── Hash table structure ──────────────────────────────────────────────
// Flat array of RCU-published bucket heads.  Grow = COW the array,
// rcu_assign_pointer, call_rcu_head old table.  Never-removed heads
// (after insert) are unlinked via rcu_assign_pointer of pred->next.

typedef struct dcache_table {
    uint32_t    cap;                  // power of two
    rcu_head_t  rcu_head;             // deferred free
    dentry_t*   buckets[];            // flexible; cap pointers
} dcache_table_t;

static dcache_table_t* g_dcache_table __attribute__((aligned(16))) = NULL;
static spinlock_t      g_dcache_wlock = SPINLOCK_INIT;
static volatile uint32_t g_dcache_live_count = 0;

// LRU: doubly-linked list of dentries with refcount == 0.  Head =
// most-recently unrefed (hot); tail = oldest (shrinker victim).
// Guarded by g_dcache_wlock to keep 7A simple — under load the LRU
// is touched on every put / shrinker / invalidate, all of which are
// cold compared to lookup.  Future: split into per-NUMA or per-CPU
// LRU shards if LRU contention becomes visible.
static dentry_t* g_dcache_lru_head = NULL;
static dentry_t* g_dcache_lru_tail = NULL;

// Stats — non-atomic owner-only where possible; hits/misses bumped
// with relaxed atomics because readers on any CPU increment.
static volatile uint64_t s_stat_hits          = 0;
static volatile uint64_t s_stat_misses        = 0;
static volatile uint64_t s_stat_negative_hits = 0;
static volatile uint64_t s_stat_installs      = 0;
static volatile uint64_t s_stat_invalidations = 0;
static volatile uint64_t s_stat_evictions     = 0;

// The dedicated slab cache for dentries.  SLAB_TYPESAFE_BY_RCU means
// a just-freed dentry's memory remains dentry_t-typed for the grace
// period — readers re-validate their hit via the (parent_ino,
// name_hash, name_len, memcmp) check so instance reuse is detected.
static slab_cache_t g_dentry_cache;

// ── Hashing ──────────────────────────────────────────────────────────
// FNV-1a over the name, then mixed with parent_ino via Knuth's
// multiplicative hash.  Inline and branchless over the fast-path
// (short names), so zero function-call overhead when the compiler
// inlines dcache_name_hash into the reader.

uint32_t dcache_name_hash(const char* name, uint32_t name_len) {
    uint32_t h = 2166136261u;  // FNV offset basis
    for (uint32_t i = 0; i < name_len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t dcache_bucket(uint32_t parent_ino, uint32_t name_hash,
                                      uint32_t cap) {
    // Mix parent_ino into name_hash via Knuth; mask to table size.
    uint32_t mixed = (parent_ino * 2654435761u) ^ name_hash;
    return mixed & (cap - 1u);
}

// Return d->name[] pointer (inline or external).
static inline const char* dname(const dentry_t* d) {
    return d->name_ext ? d->name_ext : d->name;
}

// Compare a dentry's name against (name, name_len).  Returns 1 on
// match, 0 otherwise.  kmemeq uses `repe cmpsb` for hardware-
// accelerated compare on modern x86.
static inline int dname_eq(const dentry_t* d, const char* name, uint32_t name_len) {
    return kmemeq(dname(d), name, name_len);
}

// ── RCU reader helpers ──────────────────────────────────────────────

static inline dcache_table_t* dcache_rcu_table(void) {
    return (dcache_table_t*)rcu_dereference(g_dcache_table);
}

// ── Init / slab cache ──────────────────────────────────────────────

static dcache_table_t* dcache_table_alloc(uint32_t cap) {
    dcache_table_t* t = (dcache_table_t*)kmalloc(
        sizeof(dcache_table_t) + (uint64_t)cap * sizeof(dentry_t*));
    if (!t) return NULL;
    t->cap = cap;
    __builtin_memset(t->buckets, 0, (uint64_t)cap * sizeof(dentry_t*));
    return t;
}

static void dcache_table_free_rcu(void* p) { kfree(p); }

void dcache_init(void) {
    // Dedicated cache with SLAB_TYPESAFE_BY_RCU.  After a dentry is
    // RCU-freed, the memory stays dentry_t-typed for the grace
    // period — readers mid-walk validate via the hash+name compare.
    pmm_slab_cache_init(&g_dentry_cache, sizeof(dentry_t),
                        SLAB_TYPESAFE_BY_RCU);

    dcache_table_t* t = dcache_table_alloc(DCACHE_HASH_INIT_CAP);
    rcu_assign_pointer(g_dcache_table, t);
}

// ── Allocation helpers ──────────────────────────────────────────────

static dentry_t* dentry_alloc(uint32_t parent_ino, const char* name,
                               uint32_t name_len, uint32_t name_hash,
                               uint32_t child_ino, dentry_t* parent) {
    dentry_t* d = (dentry_t*)pmm_slab_alloc(&g_dentry_cache);
    if (!d) return NULL;

    d->parent_ino = parent_ino;
    d->child_ino  = child_ino;
    d->name_hash  = name_hash;
    d->name_len   = (uint16_t)name_len;
    d->_pad0      = 0;
    d->hash_next  = NULL;
    d->parent     = parent;
    d->refcount   = 1;                  // caller gets initial reference
    d->last_used_ns = tsc_read_ns();
    d->lru_prev   = NULL;
    d->lru_next   = NULL;

    if (name_len < DNAME_INLINE_MAX) {
        __builtin_memcpy(d->name, name, name_len);
        d->name[name_len] = '\0';
        d->name_ext = NULL;
    } else {
        d->name[0] = '\0';
        d->name_ext = (char*)kmalloc(name_len + 1);
        if (!d->name_ext) { pmm_slab_free(d); return NULL; }
        __builtin_memcpy(d->name_ext, name, name_len);
        d->name_ext[name_len] = '\0';
    }

    // Hold a refcount on the parent dentry for rmdir-prune safety.
    // (Dropped in dentry_free_cb below.)
    if (parent) {
        __atomic_fetch_add(&parent->refcount, 1u, __ATOMIC_ACQ_REL);
    }
    return d;
}

// Append an UNLINKED dentry at the LRU tail (mirror of lru_push_head_locked).
// Caller holds g_dcache_wlock and has verified d is not already on the list.
// Both put-to-LRU sites (dentry_free_cb, dcache_put) inlined this exact splice.
static inline void lru_push_tail_locked(dentry_t* d) {
    d->lru_next = NULL;
    d->lru_prev = g_dcache_lru_tail;
    if (g_dcache_lru_tail) g_dcache_lru_tail->lru_next = d;
    else                   g_dcache_lru_head = d;
    g_dcache_lru_tail = d;
}

// RCU callback: actually free a dentry.  At this point no reader
// can still be traversing it (grace period elapsed).
static void dentry_free_cb(void* p) {
    dentry_t* d = (dentry_t*)p;
    if (d->name_ext) kfree(d->name_ext);

    // Drop the parent refcount we took at alloc time.  If the
    // parent's refcount hits zero, it joins the LRU — don't free
    // synchronously (the shrinker handles reclaim).
    if (d->parent) {
        uint32_t pr = __atomic_sub_fetch(&d->parent->refcount, 1u,
                                          __ATOMIC_ACQ_REL);
        if (pr == 0) {
            uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
            // Put on LRU tail (we hold refs, so this shouldn't
            // happen mid-walk; defensive).  If we race with another
            // put the other side will skip us (we're not linked).
            if (d->parent->lru_prev == NULL && d->parent->lru_next == NULL
                && g_dcache_lru_head != d->parent
                && g_dcache_lru_tail != d->parent) {
                lru_push_tail_locked(d->parent);
            }
            spin_unlock_irqrestore(&g_dcache_wlock, f);
        }
    }

    pmm_slab_free(d);
    __atomic_fetch_sub(&g_dcache_live_count, 1u, __ATOMIC_RELAXED);
}

// ── LRU helpers (caller holds g_dcache_wlock) ───────────────────────

static inline void lru_unlink_locked(dentry_t* d) {
    if (d->lru_prev) d->lru_prev->lru_next = d->lru_next;
    else if (g_dcache_lru_head == d) g_dcache_lru_head = d->lru_next;
    if (d->lru_next) d->lru_next->lru_prev = d->lru_prev;
    else if (g_dcache_lru_tail == d) g_dcache_lru_tail = d->lru_prev;
    d->lru_prev = d->lru_next = NULL;
}

static inline void lru_push_head_locked(dentry_t* d) {
    d->lru_prev = NULL;
    d->lru_next = g_dcache_lru_head;
    if (g_dcache_lru_head) g_dcache_lru_head->lru_prev = d;
    else                   g_dcache_lru_tail = d;
    g_dcache_lru_head = d;
}

// ── Lookup (hot path) ───────────────────────────────────────────────

// Sentinel refcount value meaning "the shrinker has claimed this dentry for
// free; do not resurrect it."  refcount is otherwise a small holder count
// (0 = reclaimable), so the top of the range is safe to reserve.
#define DCACHE_REF_DYING 0xFFFFFFFFu

dentry_t* dcache_lookup(uint32_t parent_ino, const char* name,
                         uint32_t name_len, uint32_t name_hash) {
    rcu_read_lock();
    dcache_table_t* tbl = dcache_rcu_table();
    if (!tbl) { rcu_read_unlock(); return NULL; }
    uint32_t b = dcache_bucket(parent_ino, name_hash, tbl->cap);

    // Walk the bucket chain lockless via rcu_dereference.  Every
    // `next` pointer is an RCU-published pointer; the iteration is
    // safe even if writers insert/remove concurrently (they do so
    // via rcu_assign_pointer + call_rcu_head-deferred free).
    dentry_t* d = (dentry_t*)rcu_dereference(tbl->buckets[b]);
    while (d) {
        if (d->parent_ino == parent_ino &&
            d->name_hash  == name_hash  &&
            d->name_len   == name_len   &&
            dname_eq(d, name, name_len)) {
            // Hit.  Bump refcount via CAS so we never resurrect a dentry
            // the shrinker has already claimed for free (refcount ==
            // DCACHE_REF_DYING).  A bare fetch_add could raise 0->1 on a
            // dentry the shrinker decided to free from its 0 snapshot,
            // leaving it referenced / on the LRU yet queued for RCU free
            // -> use-after-free.  The CAS makes "claim for free" (shrinker)
            // and "resurrect" (here) mutually exclusive on the refcount.
            uint32_t rc = __atomic_load_n(&d->refcount, __ATOMIC_ACQUIRE);
            while (rc != DCACHE_REF_DYING &&
                   !__atomic_compare_exchange_n(&d->refcount, &rc, rc + 1u, 0,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                ;  // cmpxchg reloads rc on failure; retry
            if (rc == DCACHE_REF_DYING) {
                // Being reclaimed — skip it and keep walking the chain
                // (a fresh dentry for the same key may have been installed).
                d = (dentry_t*)rcu_dereference(d->hash_next);
                continue;
            }

            // Update LRU age — relaxed store, pure hint.  Not a
            // full LRU bump (which requires g_dcache_wlock); the
            // shrinker uses last_used_ns to pick candidates.
            __atomic_store_n(&d->last_used_ns, tsc_read_ns(),
                              __ATOMIC_RELAXED);

            if (d->child_ino == DCACHE_NEGATIVE) {
                __atomic_fetch_add(&s_stat_negative_hits, 1u,
                                    __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_add(&s_stat_hits, 1u, __ATOMIC_RELAXED);
            }
            rcu_read_unlock();
            return d;
        }
        d = (dentry_t*)rcu_dereference(d->hash_next);
    }
    rcu_read_unlock();
    __atomic_fetch_add(&s_stat_misses, 1u, __ATOMIC_RELAXED);
    return NULL;
}

// ── Put (drop a reference) ──────────────────────────────────────────

void dcache_put(dentry_t* d) {
    if (!d) return;
    uint32_t new_ref = __atomic_sub_fetch(&d->refcount, 1u, __ATOMIC_ACQ_REL);
    if (new_ref != 0) return;

    // Refcount hit zero — put on LRU tail (coldest).  Writer lock
    // serialises the LRU list.  LRU push is a handful of pointer
    // assignments.
    uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
    // Another put may race us; re-check refcount under the lock.
    if (__atomic_load_n(&d->refcount, __ATOMIC_ACQUIRE) == 0
        && d->lru_prev == NULL && d->lru_next == NULL
        && g_dcache_lru_head != d && g_dcache_lru_tail != d) {
        // Append to tail — cold.  Hits bump last_used_ns but don't
        // relink (would require the writer lock).  The shrinker
        // picks by last_used_ns among the LRU's candidates.
        lru_push_tail_locked(d);
    }
    spin_unlock_irqrestore(&g_dcache_wlock, f);
}

// ── Install (writer) ────────────────────────────────────────────────

// Writer-side search: walks a bucket looking for (parent_ino, name).
// Caller holds g_dcache_wlock.  Plain pointer loads (no rcu_dereference
// needed under write lock).
static dentry_t* bucket_find_locked(dcache_table_t* tbl, uint32_t b,
                                     uint32_t parent_ino, const char* name,
                                     uint32_t name_len, uint32_t name_hash) {
    for (dentry_t* d = tbl->buckets[b]; d; d = d->hash_next) {
        if (d->parent_ino == parent_ino &&
            d->name_hash  == name_hash  &&
            d->name_len   == name_len   &&
            dname_eq(d, name, name_len))
            return d;
    }
    return NULL;
}

// Grow the table to new_cap (double current).  Caller holds
// g_dcache_wlock.  Rebuilds bucket chains; publishes via
// rcu_assign_pointer; defers old-table free.
static void dcache_grow_locked(uint32_t new_cap) {
    dcache_table_t* old = g_dcache_table;
    dcache_table_t* neu = dcache_table_alloc(new_cap);
    if (!neu) return;

    // Rehash every existing dentry.  We must NOT mutate old chain
    // pointers — they're still live for concurrent readers via the
    // old table pointer until the RCU grace period closes.  Instead
    // we build a parallel copy: each dentry gets a new `hash_next`
    // in the new table.
    //
    // Since a dentry's hash_next is a single pointer, we can't
    // share it across two tables.  We split: during grow we detach
    // a dentry from the old chain, re-link into the new chain.
    // Readers on the old table pointer still see the old chain as
    // it was AT the moment they rcu_dereference'd buckets[b] — they
    // may observe a partially-emptied chain but will stop at the
    // first NULL they reach.  That's a missed lookup; they fall
    // through to the install path which re-grabs the wlock and
    // finds the entry in the new table (since we hold the wlock
    // during grow, their install is serialised behind us).
    //
    // Net effect: a momentary miss-rate blip during grow, but no
    // corruption.  Grow is rare (≤ log2(DCACHE_MAX_ENTRIES) = ~13).

    if (old) {
        for (uint32_t ob = 0; ob < old->cap; ob++) {
            dentry_t* d = old->buckets[ob];
            old->buckets[ob] = NULL;
            while (d) {
                dentry_t* next = d->hash_next;
                uint32_t nb = dcache_bucket(d->parent_ino, d->name_hash,
                                             neu->cap);
                d->hash_next = neu->buckets[nb];
                neu->buckets[nb] = d;
                d = next;
            }
        }
    }

    rcu_assign_pointer(g_dcache_table, neu);
    if (old) call_rcu_head(&old->rcu_head, dcache_table_free_rcu, old);
}

dentry_t* dcache_install(dentry_t* parent, uint32_t parent_ino,
                          const char* name, uint32_t name_len,
                          uint32_t name_hash, uint32_t child_ino) {
    if (name_len == 0 || name_len > 255) return NULL;

    // Phase 7D: enforce the soft cap before allocating.  If we're
    // over the limit, synchronously evict a chunk — we don't want
    // the cache to grow unbounded between shrinker ticks when a
    // workload touches thousands of unique paths.
    if (__atomic_load_n(&g_dcache_live_count, __ATOMIC_RELAXED)
        >= DCACHE_MAX_ENTRIES) {
        dcache_shrink(DCACHE_MAX_ENTRIES / 8);   // ~12.5% trim
    }

    // Allocate the candidate OUTSIDE the lock — pmm_slab_alloc may
    // take g_pmm_lock and we don't want lock-order inversions.
    dentry_t* d = dentry_alloc(parent_ino, name, name_len, name_hash,
                                child_ino, parent);
    if (!d) return NULL;

    uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
    dcache_table_t* tbl = g_dcache_table;

    // Check if something already exists.  If so, keep the existing
    // entry and bump its refcount (the caller gets one reference).
    uint32_t b = dcache_bucket(parent_ino, name_hash, tbl->cap);
    dentry_t* existing = bucket_find_locked(tbl, b, parent_ino, name,
                                             name_len, name_hash);
    if (existing) {
        // Take the caller's reference on `existing` UNDER g_dcache_wlock,
        // BEFORE releasing it.  The shrinker claims a dentry for free by
        // CAS'ing its refcount 0 -> DCACHE_REF_DYING AND unlinking it ALL
        // under this same lock (dcache_shrink), so a still-linked entry found
        // here is never DYING and cannot be claimed while we hold the lock --
        // the bump and the shrinker's claim are mutually exclusive.  The old
        // code bumped AFTER the unlock: the shrinker could CAS 0 -> DYING and
        // RCU-free `existing` in that gap, and the bare fetch_add then wrapped
        // DYING (0xFFFFFFFF) -> 0 on a freed slot = use-after-free.  (Unlike
        // the lock-free dcache_lookup, install holds the lock, so the lock --
        // not a DYING-aware CAS -- is the serialization here.)
        __atomic_fetch_add(&existing->refcount, 1u, __ATOMIC_ACQ_REL);
        spin_unlock_irqrestore(&g_dcache_wlock, f);
        // Dispose of our candidate; it was never visible.
        if (d->name_ext) kfree(d->name_ext);
        if (parent) __atomic_fetch_sub(&parent->refcount, 1u,
                                         __ATOMIC_ACQ_REL);
        pmm_slab_free(d);
        return existing;
    }

    // Grow if load > 75%.
    if ((g_dcache_live_count + 1u) * 4u >= tbl->cap * 3u) {
        dcache_grow_locked(tbl->cap * 2u);
        tbl = g_dcache_table;
        b = dcache_bucket(parent_ino, name_hash, tbl->cap);
    }

    // Insert at bucket head — readers walking the chain see either
    // the pre-insert head (miss) or our new node first (hit).
    d->hash_next = tbl->buckets[b];
    rcu_assign_pointer(tbl->buckets[b], d);
    __atomic_fetch_add(&g_dcache_live_count, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_stat_installs, 1u, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_dcache_wlock, f);
    return d;
}

// ── Invalidate ──────────────────────────────────────────────────────

// Core: unlink a specific dentry from its bucket chain under the
// wlock, drop from LRU, defer free via call_rcu_head.
static void dentry_unlink_and_free_locked(dcache_table_t* tbl, dentry_t* d) {
    uint32_t b = dcache_bucket(d->parent_ino, d->name_hash, tbl->cap);
    dentry_t** pp = &tbl->buckets[b];
    while (*pp && *pp != d) pp = &(*pp)->hash_next;
    if (*pp) {
        rcu_assign_pointer(*pp, d->hash_next);
    }
    lru_unlink_locked(d);
    __atomic_fetch_add(&s_stat_invalidations, 1u, __ATOMIC_RELAXED);
    call_rcu_head(&d->rcu_head, dentry_free_cb, d);
}

// Invalidate (writer holds g_dcache_wlock): make future lookups miss, and free
// the dentry -- but ONLY if no path walk currently holds it.  The old code
// call_rcu-freed UNCONDITIONALLY, so a dentry that dcache_lookup handed out
// (refcount-pinned, NOT rcu-pinned -- dcache_lookup rcu_read_unlocks before
// returning) could be freed while its holder still read child_ino + dcache_put'd
// it -> a freed read + a freed refcount-- WRITE into a reused SLAB_TYPESAFE_BY_RCU
// slot (cascading dentry UAF + LRU corruption).  Mirror the shrinker's discipline:
// CAS refcount 0 -> DYING claims an UNREFERENCED dentry (the serialization point
// vs a concurrent dcache_put's last-ref LRU-add) and frees it immediately, as
// before.  If the CAS fails the dentry is HELD: do NOT free it -- just unlink it
// from the hash (the invalidate's real purpose: future lookups miss and
// re-resolve), and let the holder's final dcache_put return it to the LRU at
// refcount 0, where the shrinker reclaims it later via its own 0 -> DYING CAS.
static void dentry_invalidate_locked(dcache_table_t* tbl, dentry_t* d) {
    uint32_t expect = 0;
    if (__atomic_compare_exchange_n(&d->refcount, &expect, DCACHE_REF_DYING,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        dentry_unlink_and_free_locked(tbl, d);   // unreferenced: free now, as before
        return;
    }
    // Held by a path walk -- hash-unlink only, never free here.
    uint32_t b = dcache_bucket(d->parent_ino, d->name_hash, tbl->cap);
    dentry_t** pp = &tbl->buckets[b];
    while (*pp && *pp != d) pp = &(*pp)->hash_next;
    if (*pp) rcu_assign_pointer(*pp, d->hash_next);
    __atomic_fetch_add(&s_stat_invalidations, 1u, __ATOMIC_RELAXED);
}

void dcache_invalidate(uint32_t parent_ino, const char* name,
                        uint32_t name_len) {
    uint32_t name_hash = dcache_name_hash(name, name_len);

    uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
    dcache_table_t* tbl = g_dcache_table;
    uint32_t b = dcache_bucket(parent_ino, name_hash, tbl->cap);
    dentry_t* d = bucket_find_locked(tbl, b, parent_ino, name,
                                      name_len, name_hash);
    if (d) dentry_invalidate_locked(tbl, d);
    spin_unlock_irqrestore(&g_dcache_wlock, f);
}

void dcache_invalidate_subtree(uint32_t dir_ino) {
    // O(n) scan of every bucket — rmdir is rare so this is fine.
    // Under the wlock.
    uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
    dcache_table_t* tbl = g_dcache_table;
    for (uint32_t i = 0; i < tbl->cap; i++) {
        dentry_t* d = tbl->buckets[i];
        while (d) {
            dentry_t* next = d->hash_next;
            if (d->parent_ino == dir_ino) {
                dentry_invalidate_locked(tbl, d);
            }
            d = next;
        }
    }
    spin_unlock_irqrestore(&g_dcache_wlock, f);
}

// ── Shrinker ────────────────────────────────────────────────────────

uint32_t dcache_shrink(uint32_t max) {
    if (max == 0) return 0;
    uint32_t evicted = 0;
    uint64_t f = spin_lock_irqsave(&g_dcache_wlock);
    dcache_table_t* tbl = g_dcache_table;
    while (evicted < max && g_dcache_lru_head) {
        dentry_t* d = g_dcache_lru_head;
        // Claim the dentry for free by CAS'ing refcount 0 -> DYING.  This
        // is the serialization point with the lock-free dcache_lookup: if a
        // looker resurrected it (CAS'd 0 -> 1) first, our CAS fails and we
        // just pull it off the LRU and skip -- never freeing a dentry a
        // looker holds (which would leave a freed slot on the LRU / hash).
        uint32_t expect = 0;
        if (!__atomic_compare_exchange_n(&d->refcount, &expect, DCACHE_REF_DYING,
                                         0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            lru_unlink_locked(d);
            continue;
        }
        dentry_unlink_and_free_locked(tbl, d);
        evicted++;
    }
    __atomic_fetch_add(&s_stat_evictions, evicted, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_dcache_wlock, f);
    return evicted;
}

// ── Stats ───────────────────────────────────────────────────────────

void dcache_stats_get(dcache_stats_t* out) {
    if (!out) return;
    out->hits          = __atomic_load_n(&s_stat_hits,          __ATOMIC_RELAXED);
    out->misses        = __atomic_load_n(&s_stat_misses,        __ATOMIC_RELAXED);
    out->negative_hits = __atomic_load_n(&s_stat_negative_hits, __ATOMIC_RELAXED);
    out->installs      = __atomic_load_n(&s_stat_installs,      __ATOMIC_RELAXED);
    out->invalidations = __atomic_load_n(&s_stat_invalidations, __ATOMIC_RELAXED);
    out->evictions     = __atomic_load_n(&s_stat_evictions,     __ATOMIC_RELAXED);
    out->live_count    = __atomic_load_n(&g_dcache_live_count,  __ATOMIC_RELAXED);
    dcache_table_t* tbl = g_dcache_table;
    out->table_cap     = tbl ? tbl->cap : 0;
}
