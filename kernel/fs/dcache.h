#pragma once
#include "common.h"
#include "rcu.h"

// ── VFS directory-entry cache (Phase 7) ────────────────────────────────
//
// Caches `(parent_inode, name) → child_inode` mappings so repeated
// path walks skip the per-component directory-block scan done by
// ext2_dir_lookup.  Every `stat`, `open`, `exec`, and `readlink`
// does a fresh path walk in the current code; after 7A+7B every
// repeated walk is served from RAM via RCU with zero atomics on the
// hot reader path.
//
// ── Design ──────────────────────────────────────────────────────────
//
// dentry_t is allocated from a dedicated SLAB_TYPESAFE_BY_RCU cache.
// After a grace period, a freed dentry's memory may be reused for a
// different (parent, name, child) tuple, but the slot is always
// `dentry_t`-typed.  Readers re-validate the identity of a hit by
// checking parent_ino + name_hash + name_len + memcmp(name, ...).
//
// The hash table `g_dcache_table` is a flat array of bucket heads,
// each head is an RCU-published singly-linked list of dentries
// chained via `hash_next`.
//
// Readers (hot path):
//   rcu_read_lock();
//   bucket = hash(parent_ino, name);
//   for (d = rcu_deref(bucket_head); d; d = rcu_deref(d->hash_next)) {
//       if (d->parent_ino == parent_ino &&
//           d->name_hash  == name_hash  &&
//           d->name_len   == name_len   &&
//           memcmp(d->name, name, name_len) == 0) {
//           child = d->child_ino;          // 0 = negative dentry (ENOENT)
//           hit;
//           break;
//       }
//   }
//   rcu_read_unlock();
//
// Writers take `g_dcache_wlock` for inserts/removes/grow.  All
// freed dentries go through `call_rcu_head`.
//
// ── LRU + eviction (7D) ─────────────────────────────────────────────
//
// Dentries with refcount == 0 live on a doubly-linked LRU list.  The
// shrinker reclaims oldest-first under memory pressure.  On lookup
// hit, the reader bumps `last_used_ns` (relaxed store — pure hint).
//
// ── Invariants ──────────────────────────────────────────────────────
//
//  - A dentry is on EXACTLY ONE of: (a) the hash bucket chain and
//    potentially the LRU list, or (b) the RCU free queue.
//  - parent_ino == 0 is reserved for the root "/": parent=NULL,
//    parent_ino = 0, name = "/", child_ino = 2 (ext2 root).
//  - child_ino == DCACHE_NEGATIVE (0xFFFFFFFF) means "this path
//    does not exist" — cached ENOENT.  parent_ino is still a valid
//    existing directory.

#define DNAME_INLINE_MAX     32u     // up to 31 chars + NUL fits inline;
                                     // longer names fall back to d->name_ext
#define DCACHE_HASH_INIT_CAP 256u    // power of two
#define DCACHE_MAX_ENTRIES   8192u   // soft cap; shrinker evicts above this
#define DCACHE_NEGATIVE      0xFFFFFFFFu

struct dentry;
typedef struct dentry dentry_t;

struct dentry {
    // ── RCU-hot read-only fields ─────────────────────────────────────
    // Access pattern: readers touch these with just rcu_dereference,
    // compare against the lookup key, and optionally read child_ino.
    // Ordered to fit the compare keys (parent_ino, name_hash, name_len)
    // in the first cache line with the inlined name.
    uint32_t     parent_ino;         // 0 = root's parent (no real parent)
    uint32_t     child_ino;          // 0 = path to be resolved,
                                     // DCACHE_NEGATIVE = cached ENOENT
    uint32_t     name_hash;          // FNV-1a over name (host-order uint32)
    uint16_t     name_len;           // excludes NUL; ≤ 255 supported
    uint16_t     _pad0;

    // hash_next is rcu_assign_pointer'd by the writer; rcu_dereference'd
    // by the reader.  A remove unlinks by rcu_assigning the predecessor.
    dentry_t*    hash_next;

    // Inline name (32 bytes).  If name_len < DNAME_INLINE_MAX the name
    // is stored here directly, NUL-terminated for debug grep.  Longer
    // names use the heap pointer — name[0] is '\0' and name_ext points
    // at the full-length buffer.  Extremely rare in practice.
    char         name[DNAME_INLINE_MAX];
    char*        name_ext;           // NULL = use `name[]` inline

    // ── Refcount + LRU ───────────────────────────────────────────────
    volatile uint32_t refcount;      // 0 = reclaimable, 1+ = held by a
                                     // caller (path walk in progress etc)
    volatile uint64_t last_used_ns;  // tsc; relaxed writes, hint only

    dentry_t*    lru_prev;
    dentry_t*    lru_next;

    // ── Parent pointer (for rmdir subtree-prune in 7C) ──────────────
    // Read-only after insert.  The parent dentry is kept alive by our
    // refcount on it at insert time; decremented at our RCU free.
    dentry_t*    parent;

    // ── RCU free ─────────────────────────────────────────────────────
    rcu_head_t   rcu_head;
};

// ── Public API ─────────────────────────────────────────────────────

void dcache_init(void);

// Compute the name hash (FNV-1a).  Exposed so callers can reuse the
// same hash value for both the lookup attempt and an eventual
// install_locked (saves recomputation).
uint32_t dcache_name_hash(const char* name, uint32_t name_len);

// Look up (parent_ino, name) in the cache.  On hit, returns a pointer
// to the dentry with its refcount incremented by one (caller MUST
// call dcache_put when done).  Returns NULL on miss.
//
// The caller checks d->child_ino:
//   0                  — never happens for a cache hit
//   DCACHE_NEGATIVE    — negative dentry, return -ENOENT
//   anything else      — positive hit, use as next parent_ino
//
// MUST be called with rcu_read_lock NOT held — internally takes it
// and releases before returning.  Refcount bump before unlock keeps
// the returned dentry alive for the caller.
dentry_t* dcache_lookup(uint32_t parent_ino, const char* name,
                         uint32_t name_len, uint32_t name_hash);

// Release a reference taken by dcache_lookup.  If refcount hits zero,
// the dentry goes onto the LRU list for eventual eviction.
void dcache_put(dentry_t* d);

// Install a new dentry mapping (parent_ino, name) → child_ino.
// If an entry already exists, this is a no-op.  If child_ino is
// DCACHE_NEGATIVE, installs a negative dentry.
//
// Returns the installed (or existing) dentry with a refcount of 1.
// Caller must call dcache_put when done.  May return NULL on OOM.
//
// `parent` is the parent dentry (may be NULL for the root or when the
// caller is operating on inode numbers without a dentry chain — the
// current ext2 path walk works this way).  When parent is NULL,
// rmdir's subtree-prune walks by parent_ino instead.
dentry_t* dcache_install(dentry_t* parent, uint32_t parent_ino,
                          const char* name, uint32_t name_len,
                          uint32_t name_hash, uint32_t child_ino);

// Invalidate `(parent_ino, name)` — matching dentry is unlinked from
// the hash bucket and scheduled for RCU-deferred free.  Safe to call
// with no matching entry (no-op).
void dcache_invalidate(uint32_t parent_ino, const char* name,
                        uint32_t name_len);

// Invalidate every dentry whose parent_ino matches `dir_ino`.  Used
// by rmdir to prune the subtree rooted at a directory whose inode
// is being removed.  Also scans negative dentries under that dir.
void dcache_invalidate_subtree(uint32_t dir_ino);

// Shrinker hook (Phase 7D): evict up to `max` oldest dentries.
// Returns number evicted.
uint32_t dcache_shrink(uint32_t max);

// Stats (Phase 7F).
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t negative_hits;
    uint64_t installs;
    uint64_t invalidations;
    uint64_t evictions;
    uint32_t live_count;
    uint32_t table_cap;
} dcache_stats_t;

void dcache_stats_get(dcache_stats_t* out);
