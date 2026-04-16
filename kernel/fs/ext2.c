#include "ext2.h"
#include "ahci.h"
#include "kheap.h"
#include "common.h"
#include "smp.h"
#include "seqlock.h"
#include "rcu.h"

// ── Multi-task / SMP-safe scratch buffers ───────────────────────────────
// Function-local 4 KiB scratch buffers cannot live in BSS (would race
// across CPUs) and shouldn't live on the kstack (8 KiB total — too tight
// for nested ext2 paths).  Use the pattern below: declare the slot once
// at the top of the function, then alloc *lazily* just before first use
// so we never waste an alloc on an early-error path.
//
//   int f(...) {
//       EXT2_SCRATCH_DECL(buf);          // declares uint8_t* buf = NULL
//       if (...error...) return 0;       // no alloc happened
//       EXT2_SCRATCH_ALLOC(buf);         // kmalloc, bails with `return 0`
//       read_block(blk, buf);
//       // ... cleanup attribute frees buf on every return path
//   }
//
// Three ALLOC variants pick the right OOM return for each helper's
// failure-sentinel convention:
//   EXT2_SCRATCH_ALLOC(name)        -> bails with `return 0`  (most helpers)
//   EXT2_SCRATCH_ALLOC_NEG1(name)   -> bails with `return -1` (vfs_read/readdir)
//   EXT2_SCRATCH_ALLOC_VOID(name)   -> bails with `return`    (free_block etc.)
//
// kfree(NULL) is a no-op so the cleanup hook fires safely whether we
// alloc'd or not.
//
// Note: the file-scope `s_bcache_data` array below stays static on
// purpose — it IS the block cache and its contents are SMP-safe via
// the per-slot pin/seqlock protocol in Phase 7.2.  Anything else that
// looks like a "shared scratch buffer" must use EXT2_SCRATCH so two
// CPUs don't trample each other.
static inline void ext2_scratch_free_(uint8_t** p) {
    if (*p) kfree(*p);
}
#define EXT2_SCRATCH_DECL(name) \
    uint8_t* name __attribute__((cleanup(ext2_scratch_free_))) = NULL
#define EXT2_SCRATCH_ALLOC(name) \
    do { name = (uint8_t*)kmalloc(4096); if (!name) return 0; } while (0)
#define EXT2_SCRATCH_ALLOC_NEG1(name) \
    do { name = (uint8_t*)kmalloc(4096); if (!name) return -1; } while (0)
#define EXT2_SCRATCH_ALLOC_VOID(name) \
    do { name = (uint8_t*)kmalloc(4096); if (!name) return; } while (0)
// Convenience: declare + alloc in one step for cases where the function
// has already passed all early-error checks before the scratch is needed.
#define EXT2_SCRATCH(name)       EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC(name)
#define EXT2_SCRATCH_NEG1(name)  EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC_NEG1(name)
#define EXT2_SCRATCH_VOID(name)  EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC_VOID(name)

// ── Mount state ────────────────────────────────────────────────────────────

static uint32_t s_part_lba        = 0;
static uint32_t s_block_size      = 0;   // bytes per block (1024, 2048, or 4096)
static uint32_t s_sectors_per_blk = 0;   // block_size / 512
static uint32_t s_inodes_per_grp  = 0;
static uint32_t s_blocks_per_grp  = 0;
static uint32_t s_inode_size      = 128; // bytes per inode on disk
static uint32_t s_num_groups      = 0;
// Per-group spinlocks: serialise inode/block bitmap RMW + BGD counter
// updates within a single group.  Two CPUs allocating in different
// groups never contend.  Allocated at mount time and never freed.
static spinlock_t* s_group_locks  = NULL;
// Filesystem-wide rename mutex.  Serialises ext2_rename against itself
// so two concurrent renames cannot observe the same file briefly at
// two names (or worse, race over the same inum across opposite
// directions).  This is the same tactic Linux uses via its per-sb
// s_vfs_rename_mutex — rename is rare enough that a single global
// spinlock is not a scalability bottleneck, and it avoids the multi-
// lock acquisition ordering complexity of dual-parent locking.
static spinlock_t  s_rename_lock  = SPINLOCK_INIT;
static uint32_t s_first_data_blk  = 0;   // superblock's s_first_data_block
static uint8_t  s_mounted         = 0;

// ── Block cache ───────────────────────────────────────────────────────────
// Direct-mapped cache: 256 slots × 4KB = 1MB BSS.  Block number % BCACHE_SIZE
// determines the slot.  Collisions evict.  Eliminates redundant disk reads
// for indirect blocks (read once per file, hit cache on every subsequent
// data block lookup) and repeated reads of the same data block.

// ── Zero-copy block cache — N-way set-associative ───────────────────────
//
// 64 sets × 4 ways = 256 slots × 4 KiB = 1 MiB BSS (same total size as the
// old direct-mapped layout).  Block → set index is `blk & (NSETS-1)`; the
// 4 ways within a set are searched linearly on lookup and LRU-evicted on
// miss.  This eliminates the direct-mapped thrash where two hot blocks
// with the same low-order bits collide forever — measured as visible
// ~0.3 s stalls on workloads like `ls /bin` where the directory inode
// block and a file inode block mapped to the same slot.
//
// Per-way state (bcache_way_t) is unchanged in protocol from the old
// direct-mapped bcache_meta_t: a trylock for writer mutual exclusion, an
// atomic `tag`, and an atomic reader `pin` count so the hot reader path
// stays zero-copy and lock-free.
//
// LRU: per-way `last_use` — a monotonically-increasing uint64 bumped on
// every hit.  On miss, eviction picks the unpinned way with the smallest
// `last_use`.  If every way is pinned we fall back to the scratch buffer
// (miss still satisfies the caller) and drop the cache update — matches
// old behaviour when a pinned slot blocks a fill.
//
// Correctness protocol (unchanged per way): pin + double-check, see the
// sequence below.  Because each way has its own meta, two different ways
// of the same set can be updated independently with no contention.
//
//   Reader (one way):              Writer (bcache_fill_way):
//     pin++                          trylock(wlock)
//     smp_mb                         if (pin != 0)           unlock & skip
//     if (tag == blk) return         save old_tag
//     pin--                          tag = INVALID      // poison
//                                    smp_mb
//                                    if (pin != 0) {    // lost the race
//                                        tag = old_tag
//                                        unlock & skip
//                                    }
//                                    memcpy(data, src)
//                                    smp_wmb
//                                    tag = blk
//                                    unlock
//
// The lookup pass over the 4 ways performs one pin++/tag-load/pin-- per
// way until the matching tag is found.  4 atomics in the miss case is a
// cheap tax compared to one disk round-trip (tens of thousands of cycles).

#define BCACHE_NSETS   64u    // power of 2
#define BCACHE_WAYS    4u
#define BCACHE_SIZE    (BCACHE_NSETS * BCACHE_WAYS)
#define BCACHE_INVALID 0xFFFFFFFFu

typedef struct {
    spinlock_t wlock;          // writer mutual exclusion (trylock only)
    uint32_t   tag;            // block number, BCACHE_INVALID = empty
    uint32_t   pin;            // atomic: number of active readers holding data[]
    uint64_t   last_use;       // LRU counter — bumped on each hit
} bcache_way_t;

static uint8_t      s_bcache_data[BCACHE_NSETS][BCACHE_WAYS][4096];
static bcache_way_t s_bcache_meta[BCACHE_NSETS][BCACHE_WAYS];
// Global monotonic LRU clock — bumped on every hit.  Atomic because many
// readers may tick it concurrently.  Overflow is safe (we compare by
// subtraction wrapping through uint64 — 2^64 hits is not reachable).
static volatile uint64_t s_bcache_clock;

// Handle returned by bcache_get.  Caller keeps it on the stack and passes
// a pointer to bcache_put when done.  `data` is NULL on I/O error.
typedef struct {
    const uint8_t* data;    // either the pinned cache way or `scratch`
    uint32_t       blk;
    uint16_t       set;     // which set the pin was taken from
    uint16_t       way;     // which way within the set
    uint8_t        pinned;  // 1 = release holds a way pin; 0 = scratch fallback
} bcache_ref_t;

static inline uint32_t bcache_set_of(uint32_t blk) {
    return blk & (BCACHE_NSETS - 1u);
}

static void bcache_init(void) {
    for (uint32_t s = 0; s < BCACHE_NSETS; s++) {
        for (uint32_t w = 0; w < BCACHE_WAYS; w++) {
            spin_lock_init(&s_bcache_meta[s][w].wlock);
            s_bcache_meta[s][w].tag      = BCACHE_INVALID;
            s_bcache_meta[s][w].pin      = 0;
            s_bcache_meta[s][w].last_use = 0;
        }
    }
}

// Publish fresh data into a specific way of a specific set.  Silently
// skips on conflict: if the way is currently pinned by any reader, the
// update is dropped and the caller's data lives only in their buffer.
static void bcache_fill_way(uint32_t set, uint32_t way, uint32_t blk,
                             const uint8_t* data, uint32_t len) {
    bcache_way_t* m = &s_bcache_meta[set][way];
    if (!spin_trylock(&m->wlock)) return;
    if (__atomic_load_n(&m->pin, __ATOMIC_ACQUIRE) != 0) {
        spin_unlock(&m->wlock);
        return;
    }
    uint32_t old_tag = m->tag;
    m->tag = BCACHE_INVALID;                      // poison first
    __atomic_thread_fence(__ATOMIC_SEQ_CST);      // poison visible before pin recheck
    if (__atomic_load_n(&m->pin, __ATOMIC_ACQUIRE) != 0) {
        // Raced with a new reader pinning the old tag.  Restore and bail.
        m->tag = old_tag;
        spin_unlock(&m->wlock);
        return;
    }
    __builtin_memcpy(s_bcache_data[set][way], data, len);
    smp_wmb();                                    // data stores retire before tag publish
    m->tag = blk;
    spin_unlock(&m->wlock);
}

// Top-level fill: pick an unpinned LRU way in `blk`'s set and publish
// there.  If no way is unpinned (4 readers on 4 different blocks, rare)
// the update is dropped — no correctness impact.
static void bcache_fill(uint32_t blk, const uint8_t* data, uint32_t len) {
    uint32_t set = bcache_set_of(blk);

    // Prefer an empty/invalid way first — no eviction cost.
    uint32_t victim = BCACHE_WAYS;
    uint64_t oldest = ~(uint64_t)0;
    for (uint32_t w = 0; w < BCACHE_WAYS; w++) {
        bcache_way_t* m = &s_bcache_meta[set][w];
        if (__atomic_load_n(&m->tag, __ATOMIC_RELAXED) == BCACHE_INVALID
            && __atomic_load_n(&m->pin, __ATOMIC_RELAXED) == 0) {
            victim = w;
            break;
        }
    }
    // Otherwise: LRU-unpinned.
    if (victim == BCACHE_WAYS) {
        for (uint32_t w = 0; w < BCACHE_WAYS; w++) {
            bcache_way_t* m = &s_bcache_meta[set][w];
            if (__atomic_load_n(&m->pin, __ATOMIC_RELAXED) != 0) continue;
            uint64_t lu = m->last_use;
            if (lu < oldest) { oldest = lu; victim = w; }
        }
    }
    if (victim >= BCACHE_WAYS) return;  // all 4 ways pinned → drop.

    bcache_fill_way(set, victim, blk, data, len);
}

// Acquire a block.  On a cache hit, `ref.data` points directly into the
// cache (pinned, no copy).  On a miss, the block is read from disk into
// `scratch` and speculatively published into the cache (which may drop
// the update on slot conflict — harmless).
//
// Returns `ref.data == NULL` on I/O error.  Caller must `bcache_put(&ref)`
// on success to drop any held pin.
static bcache_ref_t bcache_get(uint32_t blk, uint8_t* scratch) {
    bcache_ref_t r = { NULL, blk, 0, 0, 0 };
    uint32_t set = bcache_set_of(blk);

    // Fast path — walk the 4 ways, optimistic pin + verify tag.
    for (uint32_t w = 0; w < BCACHE_WAYS; w++) {
        bcache_way_t* m = &s_bcache_meta[set][w];
        __atomic_fetch_add(&m->pin, 1u, __ATOMIC_ACQ_REL);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&m->tag, __ATOMIC_ACQUIRE) == blk) {
            // Hit.  Bump LRU and return the pinned way.
            m->last_use = __atomic_add_fetch(&s_bcache_clock, 1u,
                                               __ATOMIC_RELAXED);
            r.data   = s_bcache_data[set][w];
            r.set    = (uint16_t)set;
            r.way    = (uint16_t)w;
            r.pinned = 1;
            return r;
        }
        __atomic_fetch_sub(&m->pin, 1u, __ATOMIC_RELEASE);
    }

    // Slow path — go to disk.
    uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
    if (!ahci_read(lba, scratch, s_sectors_per_blk)) return r;
    bcache_fill(blk, scratch, s_block_size);
    r.data = scratch;
    return r;
}

static void bcache_put(bcache_ref_t* r) {
    if (r->pinned) {
        __atomic_fetch_sub(&s_bcache_meta[r->set][r->way].pin, 1u,
                           __ATOMIC_RELEASE);
        r->pinned = 0;
    }
    r->data = NULL;
}


// ── Inode cache — 2-level radix tree, per-leaf seqlock ──────────────────
// Key: uint32_t inode number.  Value: ext2_inode_t (cached from disk).
// L0 index = ino >> 8 ; L1 index = ino & 0xFF
// Nodes are allocated on demand via kmalloc; never freed (kernel lifetime).
//
// SMP concurrency model:
//   - Each leaf carries its own seqlock.  Readers (irtree_get) loop:
//     seq_begin → memcpy(128 B) → seq_retry.  Zero atomics on a
//     consistent read; on a rare collision they redo the memcpy.
//     They NEVER block on a writer — that's the entire point of using
//     a seqlock here over a spinlock.
//   - Writers use inode_lock / inode_unlock which call seq_write_begin
//     / seq_write_end on the leaf.  Two CPUs writing different inodes
//     never contend.  Two CPUs writing the SAME inode serialise on the
//     leaf's internal write_lock for tens of nanoseconds.
//   - The L0 / L1 pointer slots are publish-once stores; lookup is a
//     plain acquire-load with no atomics on the hot path.  Allocation
//     of a fresh leaf the first time we touch a given inode is
//     serialised by `s_irtree_alloc_lock`, which only fires once per
//     unique inode for the lifetime of the kernel.
//
// The leaf's `inode` field doubles as the in-memory authoritative copy
// of the inode for read-modify-write sequences: callers do
// `leaf = inode_lock(ino)` (which fault-loads the disk image into the
// leaf if needed), mutate `leaf->inode`, then call inode_writeback() to
// push the change to disk before inode_unlock.  The seqcount bump on
// the unlock side is what lets concurrent irtree_get readers notice the
// change without taking any lock.

#define IRTREE_BITS  8
#define IRTREE_SIZE  (1u << IRTREE_BITS)   // 256
#define IRTREE_MASK  (IRTREE_SIZE - 1u)

typedef struct {
    seqlock_t    seq;             // per-inode seqlock; readers never block
    ext2_inode_t inode;
    uint32_t     ino;
    uint8_t      valid;
} irtree_leaf_t;

typedef struct {
    irtree_leaf_t* leaves[IRTREE_SIZE];
} irtree_l1_t;

static irtree_l1_t* s_irtree[IRTREE_SIZE];  // L0: 256 pointers
// Single allocation lock — only taken the first time a given inode is
// touched.  Two CPUs racing to add the same leaf both end up sharing one.
static spinlock_t   s_irtree_alloc_lock = SPINLOCK_INIT;

// Lookup: plain acquire-loads down the L0 / L1 chain.  No lock needed
// because every store in irtree_alloc is publish-once with release
// semantics; we either see the leaf or we don't.
static irtree_leaf_t* irtree_lookup(uint32_t ino) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;
    irtree_l1_t* l1tab = __atomic_load_n(&s_irtree[l0], __ATOMIC_ACQUIRE);
    if (!l1tab) return NULL;
    irtree_leaf_t* leaf = __atomic_load_n(&l1tab->leaves[l1], __ATOMIC_ACQUIRE);
    if (!leaf || leaf->ino != ino) return NULL;
    return leaf;
}

// Allocate a fresh leaf if none exists yet.  Idempotent.  Returns NULL
// on OOM.
static irtree_leaf_t* irtree_alloc(uint32_t ino) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;

    spin_lock(&s_irtree_alloc_lock);
    irtree_l1_t* l1tab = s_irtree[l0];
    if (!l1tab) {
        l1tab = kmalloc(sizeof(irtree_l1_t));
        if (!l1tab) { spin_unlock(&s_irtree_alloc_lock); return NULL; }
        for (uint32_t i = 0; i < IRTREE_SIZE; i++) l1tab->leaves[i] = NULL;
        __atomic_store_n(&s_irtree[l0], l1tab, __ATOMIC_RELEASE);
    }
    irtree_leaf_t* leaf = l1tab->leaves[l1];
    if (!leaf) {
        leaf = kmalloc(sizeof(irtree_leaf_t));
        if (!leaf) { spin_unlock(&s_irtree_alloc_lock); return NULL; }
        seqlock_init(&leaf->seq);
        leaf->ino   = ino;
        leaf->valid = 0;
        __atomic_store_n(&l1tab->leaves[l1], leaf, __ATOMIC_RELEASE);
    }
    spin_unlock(&s_irtree_alloc_lock);
    return leaf;
}

// Forward declaration: read the inode from disk into the leaf cache.
// Defined further down once read_block / read_bgd are in scope.
static uint8_t inode_load(irtree_leaf_t* leaf, uint32_t ino);

// Public per-inode write-side lock: returns the leaf with its seqlock
// held in WRITE state.  Mutators bracket their entire RMW sequence in
// inode_lock / inode_unlock.  If the leaf is not yet populated the
// inode is loaded from disk under the lock so the caller always sees a
// valid `leaf->inode`.
static irtree_leaf_t* inode_lock(uint32_t ino) {
    irtree_leaf_t* leaf = irtree_lookup(ino);
    if (!leaf) leaf = irtree_alloc(ino);
    if (!leaf) return NULL;
    seq_write_begin(&leaf->seq);
    if (!leaf->valid) {
        if (!inode_load(leaf, ino)) {
            seq_write_end(&leaf->seq);
            return NULL;
        }
    }
    return leaf;
}

static void inode_unlock(irtree_leaf_t* leaf) {
    if (leaf) seq_write_end(&leaf->seq);
}

// Brief-section reader: copy the cached inode out under a seqlock retry
// loop.  No atomics on a consistent read.  Loops on the rare collision
// where a writer is mid-update.
static uint8_t irtree_get(uint32_t ino, ext2_inode_t* out) {
    irtree_leaf_t* leaf = irtree_lookup(ino);
    if (!leaf) return 0;
    uint32_t s;
    uint8_t valid;
    do {
        s = seq_begin(&leaf->seq);
        valid = leaf->valid && leaf->ino == ino;
        if (valid) *out = leaf->inode;
    } while (seq_retry(&leaf->seq, s));
    return valid;
}


// ── Low-level block I/O ────────────────────────────────────────────────────

// Copy-based read shim for callers that need a mutable scratch buffer
// (bitmap walks, in-place inode edits, etc.).  Goes through bcache_get so
// a hit still touches the cache once — but the caller is going to modify
// `buf`, so we pay the 4 KiB memcpy to avoid corrupting the shared slot.
static uint8_t read_block(uint32_t blk, uint8_t* buf) {
    bcache_ref_t r = bcache_get(blk, buf);
    if (!r.data) return 0;
    if (r.pinned) {
        __builtin_memcpy(buf, r.data, s_block_size);
        bcache_put(&r);
    }
    // If not pinned, r.data == buf (disk was read straight into it).
    return 1;
}

// Write one filesystem block from `buf`.  Writes through to disk and
// publishes the new contents into the block cache (skipped on slot
// conflict — harmless, the next read will re-fetch).
static uint8_t write_block(uint32_t blk, const uint8_t* buf) {
    uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
    if (!ahci_write(lba, buf, s_sectors_per_blk)) return 0;
    bcache_fill(blk, buf, s_block_size);
    return 1;
}

// ── Bitmap helpers ─────────────────────────────────────────────────────────

// Find first zero bit in `bitmap` (up to max_bits bits wide). Returns bit
// index, or UINT32_MAX if none free.
static uint32_t bitmap_find_free(const uint8_t* bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; i++) {
        uint32_t byte = i >> 3;
        uint32_t bit  = i & 7;
        if (!(bitmap[byte] & (1u << bit)))
            return i;
    }
    return UINT32_MAX;
}

static void bitmap_set(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void bitmap_clear(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static uint8_t bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit >> 3] >> (bit & 7)) & 1u;
}

// ── BGD helpers ────────────────────────────────────────────────────────────

// BGD table lives at block (s_first_data_blk + 1).
static uint32_t bgd_table_block(void) {
    return s_first_data_blk + 1;
}

// Read BGD for group `g` into `out`.  Zero-copy: we pin the slot just
// long enough to copy 32 bytes out.
// Read a BGD descriptor.  Zero-copy: pin the cache slot just long
// enough to copy 32 bytes out.  On a miss the per-call kmalloc'd
// scratch absorbs the disk read — no shared static buffer.
static uint8_t read_bgd(uint32_t g, ext2_bgd_t* out) {
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    EXT2_SCRATCH(scratch);
    bcache_ref_t r = bcache_get(blk_idx, scratch);
    if (!r.data) return 0;
    __builtin_memcpy(out, r.data + off, sizeof(ext2_bgd_t));
    bcache_put(&r);
    return 1;
}

// Write BGD for group `g` from `in`.  Read-modify-write of one block.
// Per-call scratch — two CPUs writing different groups never share the
// scratch and so never corrupt each other.  (Two CPUs writing the SAME
// group are serialised by the per-group lock added in Phase 8C-2.)
static uint8_t write_bgd(uint32_t g, const ext2_bgd_t* in) {
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    EXT2_SCRATCH(scratch);
    if (!read_block(blk_idx, scratch)) return 0;
    __builtin_memcpy(scratch + off, in, sizeof(ext2_bgd_t));
    return write_block(blk_idx, scratch);
}

// ── Inode I/O ──────────────────────────────────────────────────────────────

// Internal: load an inode from disk into the leaf cache.  Caller MUST
// hold the leaf's seqlock writer side (i.e. be inside an inode_lock /
// inode_unlock bracket OR be irtree_put_locked).  Returns 0 on disk
// error.
static uint8_t inode_load(irtree_leaf_t* leaf, uint32_t ino) {
    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    EXT2_SCRATCH(scratch);
    bcache_ref_t r = bcache_get(blk_idx, scratch);
    if (!r.data) return 0;
    __builtin_memcpy(&leaf->inode, r.data + off, sizeof(ext2_inode_t));
    bcache_put(&r);

    leaf->ino   = ino;
    leaf->valid = 1;
    return 1;
}

// Public read: fast path is the seqlock reader (no atomics).  Slow
// path takes the writer lock once to fault-load from disk.
static uint8_t read_inode(uint32_t ino, ext2_inode_t* out) {
    if (ino == 0) return 0;
    if (irtree_get(ino, out)) return 1;            // cache hit

    irtree_leaf_t* leaf = inode_lock(ino);          // loads from disk
    if (!leaf) return 0;
    *out = leaf->inode;
    inode_unlock(leaf);
    return 1;
}

// Internal disk writeback shared between inode_writeback (called with
// the leaf seqlock held) and the legacy write_inode wrapper (which
// takes the seqlock itself).  Does NOT touch the irtree cache.
static uint8_t inode_disk_write(uint32_t ino, const ext2_inode_t* in) {
    if (ino == 0) return 0;
    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    EXT2_SCRATCH(scratch);
    if (!read_block(blk_idx, scratch)) return 0;
    __builtin_memcpy(scratch + off, in, sizeof(ext2_inode_t));
    return write_block(blk_idx, scratch);
}

// Push the leaf's in-memory inode to disk.  Caller MUST hold the leaf
// seqlock writer side (via inode_lock).  This is the only way to
// persist an inode change after Phase 8C — the leaf cache already
// holds the authoritative copy, so no extra memcpy or irtree_put is
// needed.
static uint8_t inode_writeback(irtree_leaf_t* leaf) {
    if (!leaf || !leaf->valid) return 0;
    return inode_disk_write(leaf->ino, &leaf->inode);
}

// ── ext2_init ──────────────────────────────────────────────────────────────

uint8_t ext2_init(uint32_t part_lba) {
    s_part_lba = part_lba;
    s_mounted  = 0;
    bcache_init();

    // Superblock is at byte offset 1024 from partition start = LBA part_lba + 2
    // (each sector is 512 bytes, so 1024 = 2 sectors).
    static uint8_t sb_buf[1024];
    if (!ahci_read(part_lba + 2, sb_buf, 2)) return 0;

    ext2_superblock_t* sb = (ext2_superblock_t*)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) return 0;

    s_block_size      = 1024u << sb->s_log_block_size;
    s_sectors_per_blk = s_block_size / 512;
    s_inodes_per_grp  = sb->s_inodes_per_group;
    s_blocks_per_grp  = sb->s_blocks_per_group;
    s_first_data_blk  = sb->s_first_data_block;
    s_inode_size      = (sb->s_rev_level >= 1 && sb->s_inode_size > 0)
                        ? sb->s_inode_size : 128;

    // Number of block groups.
    s_num_groups = (sb->s_blocks_count + s_blocks_per_grp - 1) / s_blocks_per_grp;

    // One spinlock per group for bitmap RMW + BGD counter updates.
    // Two CPUs allocating in different groups never contend.  Allocated
    // once, never freed (kernel lifetime).
    s_group_locks = (spinlock_t*)kmalloc(s_num_groups * sizeof(spinlock_t));
    if (!s_group_locks) return 0;
    for (uint32_t g = 0; g < s_num_groups; g++)
        spin_lock_init(&s_group_locks[g]);

    s_mounted = 1;
    return 1;
}

// ── String helpers ─────────────────────────────────────────────────────────

static uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int str_cmp_n(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

// ── Block address resolution (direct + single indirect) ───────────────────

// Return the block number for the Nth logical block of an inode.
// Handles direct blocks (0-11) and single-indirect block (12-267) only.
// Returns 0 on error or if block is sparse/unallocated.
//
// Zero-copy: we only need 4 bytes out of each indirect block, so
// `bcache_get` pins the slot, we load the index, `bcache_put`.  On a
// cache hit the whole operation is 2 atomic ops and a 4-byte load —
// no 4 KiB memcpy at all.
static uint32_t inode_get_block(const ext2_inode_t* ino, uint32_t idx) {
    if (idx < 12) return ino->i_block[idx];

    // Single indirect.
    idx -= 12;
    uint32_t addrs_per_blk = s_block_size / 4;
    if (idx < addrs_per_blk) {
        uint32_t indirect_blk = ino->i_block[12];
        if (!indirect_blk) return 0;

        EXT2_SCRATCH(ind_buf);
        bcache_ref_t r = bcache_get(indirect_blk, ind_buf);
        if (!r.data) return 0;
        uint32_t out = ((const uint32_t*)r.data)[idx];
        bcache_put(&r);
        return out;
    }

    // Double indirect.
    idx -= addrs_per_blk;
    if (idx < addrs_per_blk * addrs_per_blk) {
        uint32_t dblind_blk = ino->i_block[13];
        if (!dblind_blk) return 0;

        uint32_t l1_idx = idx / addrs_per_blk;
        uint32_t l2_idx = idx % addrs_per_blk;

        EXT2_SCRATCH(dbl_buf);
        bcache_ref_t r1 = bcache_get(dblind_blk, dbl_buf);
        if (!r1.data) return 0;
        uint32_t l2_blk = ((const uint32_t*)r1.data)[l1_idx];
        bcache_put(&r1);
        if (!l2_blk) return 0;

        EXT2_SCRATCH(dbl_buf2);
        bcache_ref_t r2 = bcache_get(l2_blk, dbl_buf2);
        if (!r2.data) return 0;
        uint32_t out = ((const uint32_t*)r2.data)[l2_idx];
        bcache_put(&r2);
        return out;
    }

    // Triple indirect not implemented — too large for our use case.
    return 0;
}

// ── Directory lookup ───────────────────────────────────────────────────────

// Find a directory entry named `name` (length `name_len`) inside the directory
// inode `dir_ino`. Returns the inode number or 0 if not found.
static uint32_t dir_lookup(const ext2_inode_t* dir_ino, const char* name, uint32_t name_len) {
    uint32_t file_size = dir_ino->i_size;
    uint32_t bytes_left = file_size;
    uint32_t blk_idx = 0;

    EXT2_SCRATCH_DECL(dir_buf);  // alloc lazily once we know there's data
    uint32_t found = 0;
    while (bytes_left > 0) {
        uint32_t blk = inode_get_block(dir_ino, blk_idx);
        blk_idx++;
        if (!blk) break;

        if (!dir_buf) EXT2_SCRATCH_ALLOC(dir_buf);

        // Zero-copy walk: pin the cache slot, walk the dirents in place.
        bcache_ref_t r = bcache_get(blk, dir_buf);
        if (!r.data) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;
        while (off + 8 <= blk_bytes) {
            const ext2_dirent_t* de = (const ext2_dirent_t*)(r.data + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                str_cmp_n(de->name, name, name_len) == 0) {
                found = de->inode;
                break;
            }
            off += de->rec_len;
        }
        bcache_put(&r);
        if (found) return found;

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }

    return 0;
}

// ── Path resolution ────────────────────────────────────────────────────────

// Resolve an absolute path to an inode number.
// Returns 0 on failure.
static uint32_t path_to_inode(const char* path) {
    if (!s_mounted) return 0;
    if (!path || path[0] == '\0') return 0;
    if (path[0] != '/') return 0; // must be absolute

    uint32_t cur_ino = EXT2_ROOT_INO;

    uint32_t i = 1; // skip leading '/'

    while (path[i] != '\0') {
        // Extract next component.
        uint32_t start = i;
        while (path[i] != '\0' && path[i] != '/') i++;
        uint32_t comp_len = i - start;
        if (comp_len == 0) {
            // trailing slash or double slash — skip
            if (path[i] == '/') { i++; continue; }
            break;
        }

        // Read current inode.
        ext2_inode_t cur_inode;
        if (!read_inode(cur_ino, &cur_inode)) return 0;
        if (!(cur_inode.i_mode & EXT2_S_IFDIR)) return 0; // not a dir

        // Find component in directory.
        cur_ino = dir_lookup(&cur_inode, path + start, comp_len);
        if (!cur_ino) return 0;

        // Skip slash.
        if (path[i] == '/') i++;
    }

    return cur_ino;
}

// Forward declarations for helpers used by ext2_vfs_write.
static uint32_t alloc_block(void);
static void     free_block(uint32_t blk);
static uint8_t  inode_set_block(ext2_inode_t* inode, uint32_t idx, uint32_t blk_num);

// ── File read VFS callbacks ────────────────────────────────────────────────

typedef struct {
    uint32_t ino;
    uint32_t cur_pos;
    uint32_t file_size;
    ext2_inode_t inode;
} ext2_fd_t;

static int64_t ext2_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd) return -1;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;

    // Partial-block fallback scratch — only allocated if we actually
    // hit a non-block-aligned read.  Most reads use the multi-block DMA
    // fast path below and never touch this buffer.
    EXT2_SCRATCH_DECL(rd_buf);

    while (total < len) {
        if (fd->cur_pos >= fd->file_size) break;

        uint32_t blk_idx     = fd->cur_pos / s_block_size;
        uint32_t off_in_blk  = fd->cur_pos % s_block_size;
        uint32_t remain_file = fd->file_size - fd->cur_pos;
        uint32_t remain_blk  = s_block_size - off_in_blk;

        uint32_t to_copy = (uint32_t)(len - total);
        if (to_copy > remain_blk)  to_copy = remain_blk;
        if (to_copy > remain_file) to_copy = remain_file;

        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            for (uint32_t i = 0; i < to_copy; i++) dst[total + i] = 0;
            total       += to_copy;
            fd->cur_pos += to_copy;
            continue;
        }

        // Multi-block fast path: if we're block-aligned and reading full
        // blocks, coalesce consecutive physical blocks into one large DMA.
        // This turns 125 separate 4KB reads into ~4 large 32KB reads.
        if (off_in_blk == 0 && to_copy == s_block_size) {
            uint32_t run = 1;
            uint32_t max_run = (len - total) / s_block_size;
            uint32_t file_blks = (fd->file_size - fd->cur_pos) / s_block_size;
            if (max_run > file_blks) max_run = file_blks;
            // DMA_SECTORS = 64, each block = 8 sectors → max 8 blocks per DMA
            uint32_t dma_max = 64 / s_sectors_per_blk;
            if (max_run > dma_max) max_run = dma_max;

            while (run < max_run) {
                uint32_t next_blk = inode_get_block(&fd->inode, blk_idx + run);
                if (next_blk != blk + run) break;
                run++;
            }

            if (run > 1) {
                // Multi-block coalesced DMA.
                uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
                uint32_t sectors = run * s_sectors_per_blk;
                uint8_t* dest = dst + total;

                // User-space buffer → scatter-gather zero-copy.
                // Kernel buffer (ELF loader, etc.) → plain ahci_read
                // (kthread can access HHDM addresses directly).
                uint8_t is_user = ((uint64_t)dest < HHDM_OFFSET);
                if (is_user) {
                    if (!ahci_read_user(lba, dest, sectors)) return -1;
                } else {
                    if (!ahci_read(lba, dest, sectors)) return -1;
                }

                // Populate the block cache from the DMA buffer.  bcache_fill
                // silently skips any slot that is currently pinned by a
                // concurrent reader — harmless, next lookup will re-read.
                for (uint32_t r = 0; r < run; r++)
                    bcache_fill(blk + r, dest + r * s_block_size, s_block_size);
                uint32_t bytes = run * s_block_size;
                total       += bytes;
                fd->cur_pos += bytes;
                continue;
            }
        }

        // Single block read (partial block or non-consecutive).  Pin
        // the cache slot, copy just `to_copy` bytes from `off_in_blk`
        // straight into the user dst — no intermediate scratch memcpy.
        if (!rd_buf) EXT2_SCRATCH_ALLOC_NEG1(rd_buf);
        bcache_ref_t r = bcache_get(blk, rd_buf);
        if (!r.data) return -1;
        __builtin_memcpy(dst + total, r.data + off_in_blk, to_copy);
        bcache_put(&r);

        total        += to_copy;
        fd->cur_pos  += to_copy;
    }

    return (int64_t)total;
}

// Write `len` bytes from `buf` at fd->cur_pos, growing the file as needed.
static int64_t ext2_vfs_write(vfs_file_t* self, const void* buf, uint64_t len) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd || !len) return 0;

    // O_APPEND: move to end before writing.
    if (self->flags & 0x400 /*O_APPEND*/) fd->cur_pos = fd->file_size;

    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;

    // wr_buf is needed for every iteration of the write loop.
    // zb is only needed when extending the file with a new block — alloc lazily.
    EXT2_SCRATCH(wr_buf);
    EXT2_SCRATCH_DECL(zb);

    while (total < len) {
        uint32_t blk_idx    = fd->cur_pos / s_block_size;
        uint32_t off_in_blk = fd->cur_pos % s_block_size;
        uint32_t to_write   = s_block_size - off_in_blk;
        if (to_write > (uint32_t)(len - total))
            to_write = (uint32_t)(len - total);

        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            blk = alloc_block();
            if (!blk) break;
            if (!zb) {
                EXT2_SCRATCH_ALLOC(zb);
                __builtin_memset(zb, 0, s_block_size);
            }
            if (!write_block(blk, zb)) { free_block(blk); break; }
            if (!inode_set_block(&fd->inode, blk_idx, blk)) { free_block(blk); break; }
        }

        // Read-modify-write if partial block.
        if (off_in_blk != 0 || to_write != s_block_size) {
            if (!read_block(blk, wr_buf)) break;
        }
        for (uint32_t i = 0; i < to_write; i++)
            wr_buf[off_in_blk + i] = src[total + i];
        if (!write_block(blk, wr_buf)) break;

        total        += to_write;
        fd->cur_pos  += to_write;
        if (fd->cur_pos > fd->file_size) fd->file_size = fd->cur_pos;
    }

    // Persist updated inode (size + block pointers).  Take the per-
    // inode seqlock so a concurrent reader sees an atomic snapshot.
    fd->inode.i_size   = fd->file_size;
    fd->inode.i_blocks = ((fd->file_size + s_block_size - 1) / s_block_size) * (s_block_size / 512);
    irtree_leaf_t* leaf = inode_lock(fd->ino);
    if (leaf) {
        leaf->inode = fd->inode;
        inode_writeback(leaf);
        inode_unlock(leaf);
    }

    return (int64_t)total;
}

static int64_t ext2_vfs_seek(vfs_file_t* self, int64_t offset, int whence) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd) return -1;
    int64_t new_pos;
    if (whence == 0 /*SEEK_SET*/) new_pos = offset;
    else if (whence == 1 /*SEEK_CUR*/) new_pos = (int64_t)fd->cur_pos + offset;
    else if (whence == 2 /*SEEK_END*/) new_pos = (int64_t)fd->file_size + offset;
    else return -1;
    if (new_pos < 0) return -1;
    fd->cur_pos = (uint32_t)new_pos;
    return new_pos;
}

static void ext2_vfs_close(vfs_file_t* self) {
    if (self->ctx) kfree(self->ctx);
    kfree(self);
}

// ── ext2_open ──────────────────────────────────────────────────────────────

vfs_file_t* ext2_open(const char* path) {
    if (!s_mounted) return NULL;

    uint32_t ino = path_to_inode(path);
    if (!ino) return NULL;

    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return NULL;

    // Must be a regular file.
    if (!(inode.i_mode & EXT2_S_IFREG)) return NULL;

    ext2_fd_t* fd = kmalloc(sizeof(ext2_fd_t));
    if (!fd) return NULL;

    fd->ino       = ino;
    fd->cur_pos   = 0;
    fd->file_size = inode.i_size;
    // Copy inode.
    uint8_t* dst = (uint8_t*)&fd->inode;
    const uint8_t* src = (const uint8_t*)&inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) dst[i] = src[i];

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(fd); return NULL; }

    f->read     = ext2_vfs_read;
    f->write    = ext2_vfs_write;   // caller enforces O_RDONLY by clearing this
    f->seek     = ext2_vfs_seek;
    f->close    = ext2_vfs_close;
    f->poll           = NULL;             // ext2 files are always ready
    f->ioctl          = NULL;
    f->ctx            = fd;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount    = 1;
    f->rights   = 0;   // stamped by sys_open after open; zero for internal opens
    // Store absolute path for fstat/ftruncate.
    uint32_t pi = 0;
    if (path) {
        while (pi < 255 && path[pi]) { f->path[pi] = path[pi]; pi++; }
    }
    f->path[pi] = '\0';
    return f;
}

// ── ext2_readdir ───────────────────────────────────────────────────────────

int ext2_readdir(const char* path, ext2_entry_t* entries, int max) {
    if (!s_mounted || !entries || max <= 0) return -1;

    uint32_t dir_ino = path_to_inode(path);
    if (!dir_ino) return -1;

    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return -1;

    int count = 0;
    uint32_t bytes_left = dir_inode.i_size;
    uint32_t blk_idx = 0;

    EXT2_SCRATCH_DECL(rdd_buf);  // alloc lazily once we know the dir is non-empty
    while (bytes_left > 0 && count < max) {
        uint32_t blk = inode_get_block(&dir_inode, blk_idx);
        blk_idx++;
        if (!blk) break;

        if (!rdd_buf) EXT2_SCRATCH_ALLOC_NEG1(rdd_buf);
        // Zero-copy dirent walk: pin the cache slot and walk dirents in
        // place.  The per-entry read_inode call pins a different slot
        // (or hits the irtree cache entirely) so there's no self-conflict.
        bcache_ref_t rr = bcache_get(blk, rdd_buf);
        if (!rr.data) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes && count < max) {
            const ext2_dirent_t* de = (const ext2_dirent_t*)(rr.data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                uint32_t nlen = de->name_len;
                // Skip "." and ".."
                int is_dot = (nlen == 1 && de->name[0] == '.');
                int is_dotdot = (nlen == 2 && de->name[0] == '.' && de->name[1] == '.');

                if (!is_dot && !is_dotdot) {
                    ext2_entry_t* out = &entries[count];
                    // Copy name.
                    for (uint32_t j = 0; j < nlen && j < 255; j++)
                        out->name[j] = de->name[j];
                    out->name[nlen < 255 ? nlen : 255] = '\0';
                    out->inode_num = de->inode;

                    // Read inode to get size and type.
                    ext2_inode_t child_ino;
                    if (read_inode(de->inode, &child_ino)) {
                        out->size   = child_ino.i_size;
                        out->is_dir = (child_ino.i_mode & EXT2_S_IFDIR) ? 1 : 0;
                    } else {
                        out->size   = 0;
                        out->is_dir = (de->file_type == EXT2_FT_DIR) ? 1 : 0;
                    }
                    count++;
                }
            }

            off += de->rec_len;
        }
        bcache_put(&rr);

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }

    return count;
}

// ── Allocation helpers ─────────────────────────────────────────────────────

// Allocate a free inode from any group.  Walks groups round-robin
// (no lock during the peek); on a candidate group we take the
// per-group spinlock, re-read the BGD under the lock, and either
// commit the allocation or release the lock and continue to the next
// group.  Two CPUs allocating in different groups never contend.
static uint32_t alloc_inode(void) {
    EXT2_SCRATCH(ibm_buf);

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd_peek;
        if (!read_bgd(g, &bgd_peek)) continue;
        if (bgd_peek.bg_free_inodes_count == 0) continue;

        spin_lock(&s_group_locks[g]);
        // Re-read the BGD under the lock — another CPU may have just
        // emptied this group between the peek and the lock acquire.
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd) || bgd.bg_free_inodes_count == 0) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        if (!read_block(bgd.bg_inode_bitmap, ibm_buf)) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        uint32_t bit = bitmap_find_free(ibm_buf, s_inodes_per_grp);
        if (bit == UINT32_MAX) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        bitmap_set(ibm_buf, bit);
        if (!write_block(bgd.bg_inode_bitmap, ibm_buf)) {
            spin_unlock(&s_group_locks[g]);
            return 0;
        }
        bgd.bg_free_inodes_count--;
        write_bgd(g, &bgd);
        spin_unlock(&s_group_locks[g]);

        return g * s_inodes_per_grp + bit + 1;
    }
    return 0;
}

// Allocate a free block from any group.  Same lock protocol as
// alloc_inode: optimistic peek, then take the per-group lock and
// re-validate before committing the bit.
static uint32_t alloc_block(void) {
    EXT2_SCRATCH(bbm_buf);

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd_peek;
        if (!read_bgd(g, &bgd_peek)) continue;
        if (bgd_peek.bg_free_blocks_count == 0) continue;

        spin_lock(&s_group_locks[g]);
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd) || bgd.bg_free_blocks_count == 0) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        if (!read_block(bgd.bg_block_bitmap, bbm_buf)) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        uint32_t bit = bitmap_find_free(bbm_buf, s_blocks_per_grp);
        if (bit == UINT32_MAX) {
            spin_unlock(&s_group_locks[g]);
            continue;
        }
        bitmap_set(bbm_buf, bit);
        if (!write_block(bgd.bg_block_bitmap, bbm_buf)) {
            spin_unlock(&s_group_locks[g]);
            return 0;
        }
        bgd.bg_free_blocks_count--;
        write_bgd(g, &bgd);
        spin_unlock(&s_group_locks[g]);

        return g * s_blocks_per_grp + bit + s_first_data_blk;
    }
    return 0;
}

// Free a block.  Takes the relevant group's spinlock for the bitmap
// RMW + BGD counter update.
static void free_block(uint32_t blk) {
    if (!blk) return;
    EXT2_SCRATCH_VOID(fbb_buf);

    uint32_t rel = (blk >= s_first_data_blk) ? (blk - s_first_data_blk) : blk;
    uint32_t g   = rel / s_blocks_per_grp;
    uint32_t bit = rel % s_blocks_per_grp;
    if (g >= s_num_groups) return;

    spin_lock(&s_group_locks[g]);
    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd) || !read_block(bgd.bg_block_bitmap, fbb_buf)) {
        spin_unlock(&s_group_locks[g]);
        return;
    }
    if (bitmap_test(fbb_buf, bit)) {
        bitmap_clear(fbb_buf, bit);
        write_block(bgd.bg_block_bitmap, fbb_buf);
        bgd.bg_free_blocks_count++;
        write_bgd(g, &bgd);
    }
    spin_unlock(&s_group_locks[g]);
}

// Free an inode.  Takes the relevant group's spinlock.
static void free_inode_num(uint32_t ino) {
    if (!ino) return;
    EXT2_SCRATCH_VOID(fib_buf);

    uint32_t g   = (ino - 1) / s_inodes_per_grp;
    uint32_t bit = (ino - 1) % s_inodes_per_grp;
    if (g >= s_num_groups) return;

    spin_lock(&s_group_locks[g]);
    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd) || !read_block(bgd.bg_inode_bitmap, fib_buf)) {
        spin_unlock(&s_group_locks[g]);
        return;
    }
    if (bitmap_test(fib_buf, bit)) {
        bitmap_clear(fib_buf, bit);
        write_block(bgd.bg_inode_bitmap, fib_buf);
        bgd.bg_free_inodes_count++;
        write_bgd(g, &bgd);
    }
    spin_unlock(&s_group_locks[g]);
}

// Zero a block on disk.
static void zero_block(uint32_t blk) {
    EXT2_SCRATCH_VOID(zb_buf);
    __builtin_memset(zb_buf, 0, s_block_size);
    write_block(blk, zb_buf);
}

// Free all data blocks of an inode (direct + single indirect only).
static void free_inode_blocks(ext2_inode_t* inode) {
    uint32_t addrs_per_blk = s_block_size / 4;

    for (uint32_t i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    if (inode->i_block[12]) {
        EXT2_SCRATCH_VOID(ind_free_buf);
        if (read_block(inode->i_block[12], ind_free_buf)) {
            uint32_t* addrs = (uint32_t*)ind_free_buf;
            for (uint32_t i = 0; i < addrs_per_blk; i++) {
                if (addrs[i]) free_block(addrs[i]);
            }
        }
        free_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }

    // Double indirect.
    if (inode->i_block[13]) {
        EXT2_SCRATCH_VOID(dind_free_buf);
        // Hoist the inner buffer out of the loop — one alloc, reused per L2 read.
        EXT2_SCRATCH_VOID(dind_free_buf2);
        if (read_block(inode->i_block[13], dind_free_buf)) {
            uint32_t* l1 = (uint32_t*)dind_free_buf;
            for (uint32_t i = 0; i < addrs_per_blk; i++) {
                if (!l1[i]) continue;
                if (read_block(l1[i], dind_free_buf2)) {
                    uint32_t* l2 = (uint32_t*)dind_free_buf2;
                    for (uint32_t j = 0; j < addrs_per_blk; j++) {
                        if (l2[j]) free_block(l2[j]);
                    }
                }
                free_block(l1[i]);
            }
        }
        free_block(inode->i_block[13]);
        inode->i_block[13] = 0;
    }

    inode->i_size   = 0;
    inode->i_blocks = 0;
}

// Set block pointer N in inode (allocates indirect block if needed).
// Returns 1 on success, 0 on failure.
static uint8_t inode_set_block(ext2_inode_t* inode, uint32_t idx, uint32_t blk_num) {
    if (idx < 12) {
        inode->i_block[idx] = blk_num;
        return 1;
    }

    uint32_t addrs_per_blk = s_block_size / 4;
    idx -= 12;

    if (idx < addrs_per_blk) {
        // Single indirect.
        EXT2_SCRATCH(si_buf);
        if (!inode->i_block[12]) {
            // Allocate indirect block.
            uint32_t ind_blk = alloc_block();
            if (!ind_blk) return 0;
            inode->i_block[12] = ind_blk;
            // Zero it.
            __builtin_memset(si_buf, 0, s_block_size);
            if (!write_block(ind_blk, si_buf)) return 0;
        }
        if (!read_block(inode->i_block[12], si_buf)) return 0;
        uint32_t* addrs = (uint32_t*)si_buf;
        addrs[idx] = blk_num;
        return write_block(inode->i_block[12], si_buf);
    }

    idx -= addrs_per_blk;

    if (idx < addrs_per_blk * addrs_per_blk) {
        // Double indirect.
        EXT2_SCRATCH(di_l1_buf);
        EXT2_SCRATCH(di_l2_buf);
        if (!inode->i_block[13]) {
            uint32_t dind_blk = alloc_block();
            if (!dind_blk) return 0;
            inode->i_block[13] = dind_blk;
            __builtin_memset(di_l1_buf, 0, s_block_size);
            if (!write_block(dind_blk, di_l1_buf)) return 0;
        }
        if (!read_block(inode->i_block[13], di_l1_buf)) return 0;
        uint32_t* l1 = (uint32_t*)di_l1_buf;
        uint32_t l1_idx = idx / addrs_per_blk;
        uint32_t l2_idx = idx % addrs_per_blk;

        if (!l1[l1_idx]) {
            uint32_t l2_blk = alloc_block();
            if (!l2_blk) return 0;
            l1[l1_idx] = l2_blk;
            if (!write_block(inode->i_block[13], di_l1_buf)) return 0;
            __builtin_memset(di_l2_buf, 0, s_block_size);
            if (!write_block(l2_blk, di_l2_buf)) return 0;
        }
        if (!read_block(l1[l1_idx], di_l2_buf)) return 0;
        uint32_t* l2 = (uint32_t*)di_l2_buf;
        l2[l2_idx] = blk_num;
        return write_block(l1[l1_idx], di_l2_buf);
    }

    return 0; // triple indirect not supported
}

// ── Directory entry manipulation ───────────────────────────────────────────

// Append a directory entry `name` → `child_ino` to directory `dir_ino_num`.
// Returns 1 on success, 0 on failure.
static uint8_t dir_add_entry(uint32_t dir_ino_num, const char* name,
                              uint32_t child_ino, uint8_t file_type) {
    // Hold the parent directory inode locked across the whole add: a
    // concurrent dir_add_entry on the same dir would race the dirent
    // walk and the i_size / i_blocks updates.  inode_lock loads the
    // on-disk inode into the leaf cache if needed and pins it under a
    // seqlock writer.
    irtree_leaf_t* dir_leaf = inode_lock(dir_ino_num);
    if (!dir_leaf) return 0;
    ext2_inode_t dir_inode = dir_leaf->inode;  // working copy
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) {
        inode_unlock(dir_leaf);
        return 0;
    }

    uint32_t name_len = str_len(name);
    // Aligned rec_len for new entry.
    uint32_t new_rec_len = 8 + name_len;
    if (new_rec_len & 3) new_rec_len = (new_rec_len | 3) + 1; // align to 4

    uint32_t blk_idx = 0;
    uint32_t bytes_left = dir_inode.i_size;

    EXT2_SCRATCH(dae_buf);  // hoisted out of the directory-walk loop
    while (1) {
        uint32_t blk;
        uint8_t new_blk = 0;

        if (bytes_left == 0) {
            // Need to allocate a new block for the directory.
            blk = alloc_block();
            if (!blk) goto fail;
            zero_block(blk);
            if (!inode_set_block(&dir_inode, blk_idx, blk)) {
                free_block(blk);
                goto fail;
            }
            dir_inode.i_size   += s_block_size;
            dir_inode.i_blocks += s_block_size / 512;
            new_blk = 1;
        } else {
            blk = inode_get_block(&dir_inode, blk_idx);
            if (!blk) break;
        }

        if (!read_block(blk, dae_buf)) goto fail;

        if (new_blk) {
            // Write entry as the only entry in this fresh block.
            ext2_dirent_t* de = (ext2_dirent_t*)dae_buf;
            de->inode     = child_ino;
            de->rec_len   = (uint16_t)s_block_size;
            de->name_len  = (uint8_t)name_len;
            de->file_type = file_type;
            for (uint32_t i = 0; i < name_len; i++) de->name[i] = name[i];
            if (!write_block(blk, dae_buf)) goto fail;
            goto success;
        }

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes) {
            ext2_dirent_t* de = (ext2_dirent_t*)(dae_buf + off);
            if (de->rec_len == 0) break;

            // Check if this entry has slack space we can use.
            uint32_t actual_len = 8 + de->name_len;
            if (actual_len & 3) actual_len = (actual_len | 3) + 1;
            uint32_t slack = de->rec_len - actual_len;

            if (de->inode == 0 && de->rec_len >= new_rec_len) {
                // Reuse deleted entry.
                de->inode     = child_ino;
                de->name_len  = (uint8_t)name_len;
                de->file_type = file_type;
                for (uint32_t i = 0; i < name_len; i++) de->name[i] = name[i];
                if (!write_block(blk, dae_buf)) goto fail;
                goto success;
            }

            if (slack >= new_rec_len) {
                // Split: shrink current entry, add new one after it.
                uint32_t old_rec = de->rec_len;
                de->rec_len = (uint16_t)actual_len;
                ext2_dirent_t* new_de = (ext2_dirent_t*)(dae_buf + off + actual_len);
                new_de->inode     = child_ino;
                new_de->rec_len   = (uint16_t)(old_rec - actual_len);
                new_de->name_len  = (uint8_t)name_len;
                new_de->file_type = file_type;
                for (uint32_t i = 0; i < name_len; i++) new_de->name[i] = name[i];
                if (!write_block(blk, dae_buf)) goto fail;
                goto success;
            }

            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;

        blk_idx++;
    }

fail:
    inode_unlock(dir_leaf);
    return 0;
success:
    // Push the local working copy into the leaf and persist to disk.
    // Both happen under the seqlock writer side held by inode_lock, so
    // a concurrent reader sees either the pre-update or post-update
    // inode but never a torn snapshot.
    dir_leaf->inode = dir_inode;
    inode_writeback(dir_leaf);
    inode_unlock(dir_leaf);
    return 1;
}

// Remove a directory entry by name from the directory `dir_ino_num`.
// Marks the entry's inode field as 0 (deleted).  Holds the parent
// directory inode locked across the whole walk so a concurrent
// dir_add_entry on the same directory cannot move dirents under us.
static uint8_t dir_remove_entry(uint32_t dir_ino_num, const char* name) {
    irtree_leaf_t* dir_leaf = inode_lock(dir_ino_num);
    if (!dir_leaf) return 0;
    ext2_inode_t dir_inode = dir_leaf->inode;

    uint32_t name_len = str_len(name);
    uint32_t bytes_left = dir_inode.i_size;
    uint32_t blk_idx = 0;
    uint8_t  ok = 0;

    EXT2_SCRATCH_DECL(dre_buf);
    while (bytes_left > 0) {
        uint32_t blk = inode_get_block(&dir_inode, blk_idx);
        blk_idx++;
        if (!blk) break;

        if (!dre_buf) {
            dre_buf = (uint8_t*)kmalloc(4096);
            if (!dre_buf) break;
        }
        if (!read_block(blk, dre_buf)) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes) {
            ext2_dirent_t* de = (ext2_dirent_t*)(dre_buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                str_cmp_n(de->name, name, name_len) == 0) {
                de->inode = 0;
                write_block(blk, dre_buf);
                ok = 1;
                goto out;
            }
            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }
out:
    inode_unlock(dir_leaf);
    return ok;
}

// ── Path utilities ─────────────────────────────────────────────────────────

// Split path into parent path and basename.
// parent_out must be >= str_len(path)+1 bytes.
// Returns the basename pointer within `path`.
static const char* path_split(const char* path, char* parent_out) {
    uint32_t len = str_len(path);
    // Find last '/'.
    int last_slash = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    if (last_slash <= 0) {
        // Parent is "/".
        parent_out[0] = '/';
        parent_out[1] = '\0';
        // Basename starts after '/' at position 0, or at 0 if no slash.
        return (last_slash == 0) ? path + 1 : path;
    }

    // Copy up to last_slash.
    for (int i = 0; i < last_slash; i++) parent_out[i] = path[i];
    parent_out[last_slash] = '\0';
    return path + last_slash + 1;
}

// ── ext2_write_file ────────────────────────────────────────────────────────

int ext2_write_file(const char* path, const uint8_t* data, uint32_t size) {
    if (!s_mounted) return 0;

    // Split into parent dir and basename.
    char parent_path[256];
    const char* basename = path_split(path, parent_path);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;

    // Check if file already exists.
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;

    uint32_t existing_ino = dir_lookup(&parent_inode, basename, str_len(basename));

    // Scratch buffer reused for every block write below.  Lazily alloc'd
    // only after we've passed all the early-error checks in each branch.
    EXT2_SCRATCH_DECL(wr_buf);

    if (existing_ino) {
        // Overwrite: free all old blocks.
        ext2_inode_t old_inode;
        if (!read_inode(existing_ino, &old_inode)) return 0;

        // Free all data blocks.
        free_inode_blocks(&old_inode);

        // Now write new data into existing inode.
        uint32_t n_blocks = (size + s_block_size - 1) / s_block_size;
        uint32_t written = 0;

        if (n_blocks > 0) EXT2_SCRATCH_ALLOC(wr_buf);
        for (uint32_t bi = 0; bi < n_blocks; bi++) {
            uint32_t blk = alloc_block();
            if (!blk) return 0;

            uint32_t to_write = size - written;
            if (to_write > s_block_size) to_write = s_block_size;

            __builtin_memcpy(wr_buf, data + written, to_write);
            __builtin_memset(wr_buf + to_write, 0, s_block_size - to_write);

            if (!write_block(blk, wr_buf)) { free_block(blk); return 0; }
            if (!inode_set_block(&old_inode, bi, blk)) { free_block(blk); return 0; }

            written += to_write;
        }

        old_inode.i_size   = size;
        old_inode.i_blocks = n_blocks * (s_block_size / 512);
        old_inode.i_mode   = EXT2_S_IFREG | 0644;
        old_inode.i_links_count = 1;
        {
            irtree_leaf_t* leaf = inode_lock(existing_ino);
            if (leaf) {
                leaf->inode = old_inode;
                inode_writeback(leaf);
                inode_unlock(leaf);
            }
        }
        return 1;
    }

    // File doesn't exist — allocate a new inode.
    uint32_t new_ino = alloc_inode();
    if (!new_ino) return 0;

    ext2_inode_t new_inode;
    // Zero it.
    uint8_t* np = (uint8_t*)&new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) np[i] = 0;

    new_inode.i_mode        = EXT2_S_IFREG | 0644;
    new_inode.i_links_count = 1;
    new_inode.i_size        = size;

    uint32_t n_blocks = (size + s_block_size - 1) / s_block_size;
    uint32_t written  = 0;

    if (n_blocks > 0 && !wr_buf) {
        wr_buf = (uint8_t*)kmalloc(4096);
        if (!wr_buf) { free_inode_num(new_ino); return 0; }
    }
    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t blk = alloc_block();
        if (!blk) { free_inode_num(new_ino); return 0; }

        // Reuse wr_buf from above — same scope, single allocation.
        uint32_t to_write = size - written;
        if (to_write > s_block_size) to_write = s_block_size;

        __builtin_memcpy(wr_buf, data + written, to_write);
        __builtin_memset(wr_buf + to_write, 0, s_block_size - to_write);

        if (!write_block(blk, wr_buf)) { free_block(blk); free_inode_num(new_ino); return 0; }
        if (!inode_set_block(&new_inode, bi, blk)) {
            free_block(blk);
            free_inode_num(new_ino);
            return 0;
        }

        written += to_write;
    }

    new_inode.i_blocks = n_blocks * (s_block_size / 512);
    {
        irtree_leaf_t* leaf = inode_lock(new_ino);
        if (leaf) {
            leaf->inode = new_inode;
            leaf->valid = 1;
            inode_writeback(leaf);
            inode_unlock(leaf);
        }
    }

    // Add directory entry in parent.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_REG_FILE)) {
        free_inode_num(new_ino);
        return 0;
    }

    return 1;
}

// ── ext2_create ───────────────────────────────────────────────────────────
// Create an empty file.  Returns 0 if the file already exists.
int ext2_create(const char* path) {
    if (!s_mounted) return 0;
    char parent_path[256];
    const char* basename = path_split(path, parent_path);
    if (!basename || basename[0] == '\0') return 0;
    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;
    // Fail if already exists.
    if (dir_lookup(&parent_inode, basename, str_len(basename))) return 0;
    // Create empty file via ext2_write_file.
    return ext2_write_file(path, (const uint8_t*)"", 0);
}

// ── ext2_truncate ─────────────────────────────────────────────────────────
// Truncate file to zero bytes.
int ext2_truncate(const char* path) {
    return ext2_write_file(path, (const uint8_t*)"", 0);
}

// ── ext2_truncate_to ──────────────────────────────────────────────────────
// Truncate (or extend) a file to exactly `length` bytes.
// If length > current size, the file is extended with zeros.
// If length == 0, equivalent to ext2_truncate.
// Returns 1 on success, 0 on failure.
int ext2_truncate_to(const char* path, uint64_t length) {
    if (!s_mounted) return 0;
    if (length == 0) return ext2_truncate(path);

    // For simplicity: read the current content, resize to `length`, write back.
    // This is O(file_size) but correct and consistent with our write model.
    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;
    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return 0;
    uint32_t cur_size = inode.i_size;

    if (length == cur_size) return 1;

    // Clamp to 32-bit (we don't support >4GiB files).
    if (length > 0xFFFFFFFFULL) return 0;
    uint32_t new_size = (uint32_t)length;

    if (new_size < cur_size) {
        // Read current content, write back shorter version.
        uint8_t* buf = kmalloc(new_size);
        if (!buf) return 0;
        vfs_file_t* f = ext2_open(path);
        if (!f) { kfree(buf); return 0; }
        int64_t n = vfs_read(f, buf, new_size);
        vfs_close(f);
        if (n < 0) { kfree(buf); return 0; }
        int r = ext2_write_file(path, buf, new_size);
        kfree(buf);
        return r;
    } else {
        // Extend: read, zero-pad, write.
        uint8_t* buf = kmalloc(new_size);
        if (!buf) return 0;
        vfs_file_t* f = ext2_open(path);
        if (!f) { kfree(buf); return 0; }
        int64_t n = vfs_read(f, buf, cur_size);
        vfs_close(f);
        if (n < 0) { kfree(buf); return 0; }
        // Zero the extended region.
        for (uint32_t i = cur_size; i < new_size; i++) buf[i] = 0;
        int r = ext2_write_file(path, buf, new_size);
        kfree(buf);
        return r;
    }
}


// ── ext2_lookup_path ──────────────────────────────────────────────
// Permission-aware path walk.  Checks execute (search) on every directory
// component traversed.  Uses the same walk as path_to_inode internally.
// cred is a const cred_t* — typed as void* to avoid the ext2.h → cred.h
// dependency (cred.h is a security layer header, not an fs header).
#include "perm.h"
#include "cred.h"
#include "errno.h"

uint32_t ext2_lookup_path(const char* path, const void* cred, int* err_out) {
    if (err_out) *err_out = 0;
    if (!s_mounted || !path || path[0] != '/') {
        if (err_out) *err_out = -ENOENT;
        return 0;
    }

    const cred_t* c = (const cred_t*)cred;
    uint32_t cur_ino = EXT2_ROOT_INO;
    uint32_t i = 1; // skip leading '/'

    while (path[i] != '\0') {
        uint32_t start = i;
        while (path[i] != '\0' && path[i] != '/') i++;
        uint32_t comp_len = i - start;
        if (comp_len == 0) {
            if (path[i] == '/') { i++; continue; }
            break;
        }

        // Check execute (search) permission on the current directory.
        ext2_inode_t cur_inode;
        if (!read_inode(cur_ino, &cur_inode)) {
            if (err_out) *err_out = -ENOENT;
            return 0;
        }
        if (!(cur_inode.i_mode & EXT2_S_IFDIR)) {
            if (err_out) *err_out = -ENOENT;
            return 0;
        }
        if (c) {
            inode_perm_t ip = {
                .uid      = cur_inode.i_uid,
                .gid      = cur_inode.i_gid,
                .mode     = cur_inode.i_mode & 0x1FF,
                .inode_nr = cur_ino,
                .dev      = 0,
                .nosuid   = 0,
            };
            int pr = vfs_check_perm(&ip, c, ACL_PERM_EXEC);
            if (pr != 0) {
                if (err_out) *err_out = pr;  // -EACCES
                return 0;
            }
        }

        cur_ino = dir_lookup(&cur_inode, path + start, comp_len);
        if (!cur_ino) {
            if (err_out) *err_out = -ENOENT;
            return 0;
        }

        if (path[i] == '/') i++;
    }

    return cur_ino;
}

uint8_t ext2_read_inode(uint32_t ino, ext2_inode_t* out) {
    return read_inode(ino, out);
}

// ── ext2_mkdir ────────────────────────────────────────────────────────────

int ext2_mkdir(const char* path) {
    if (!s_mounted) return 0;

    // Check it doesn't already exist.
    if (path_to_inode(path)) return 0; // already exists

    char md_parent[256];
    const char* basename = path_split(path, md_parent);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(md_parent);
    if (!parent_ino) return 0;

    // Allocate inode.
    uint32_t new_ino = alloc_inode();
    if (!new_ino) return 0;

    // Allocate one data block for "." and "..".
    uint32_t data_blk = alloc_block();
    if (!data_blk) { free_inode_num(new_ino); return 0; }

    // Build the data block with "." and "..".  Manual alloc so we can
    // unwind the inode + block allocations on OOM instead of leaking them.
    EXT2_SCRATCH_DECL(mkdir_buf);
    mkdir_buf = (uint8_t*)kmalloc(4096);
    if (!mkdir_buf) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }
    __builtin_memset(mkdir_buf, 0, s_block_size);

    // "." entry.
    ext2_dirent_t* dot = (ext2_dirent_t*)mkdir_buf;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    // ".." entry.
    ext2_dirent_t* dotdot = (ext2_dirent_t*)(mkdir_buf + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(s_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    if (!write_block(data_blk, mkdir_buf)) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }

    // Create inode.
    ext2_inode_t new_inode;
    uint8_t* np2 = (uint8_t*)&new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) np2[i] = 0;

    new_inode.i_mode        = EXT2_S_IFDIR | 0755;
    new_inode.i_links_count = 2; // "." and parent's entry
    new_inode.i_size        = s_block_size;
    new_inode.i_blocks      = s_block_size / 512;
    new_inode.i_block[0]    = data_blk;
    {
        irtree_leaf_t* leaf = inode_lock(new_ino);
        if (leaf) {
            leaf->inode = new_inode;
            leaf->valid = 1;
            inode_writeback(leaf);
            inode_unlock(leaf);
        }
    }

    // Add to parent directory.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_DIR)) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }

    // Increment parent's links_count for ".." — RMW under per-inode lock.
    {
        irtree_leaf_t* pleaf = inode_lock(parent_ino);
        if (pleaf) {
            pleaf->inode.i_links_count++;
            inode_writeback(pleaf);
            inode_unlock(pleaf);
        }
    }

    // Increment used_dirs_count in the new inode's BGD, under the
    // per-group lock so it doesn't race a concurrent alloc_inode in
    // the same group.
    uint32_t g = (new_ino - 1) / s_inodes_per_grp;
    if (g < s_num_groups) {
        spin_lock(&s_group_locks[g]);
        ext2_bgd_t bgd;
        if (read_bgd(g, &bgd)) {
            bgd.bg_used_dirs_count++;
            write_bgd(g, &bgd);
        }
        spin_unlock(&s_group_locks[g]);
    }

    return 1;
}

// ── ext2_unlink ───────────────────────────────────────────────────────────
// Remove a regular file at `path`.  Returns 1 on success, 0 on failure.
int ext2_unlink(const char* path) {
    if (!s_mounted || !path) return 0;

    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;

    char ul_parent[256];
    const char* basename = path_split(path, ul_parent);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(ul_parent);
    if (!parent_ino) return 0;

    // Lock the target inode for the whole link-count decrement +
    // possible free.  A concurrent open() that snapshots inode.i_dtime
    // before our writeback would either see the old dtime (live) or the
    // new dtime (deleted) atomically — never a torn struct.
    irtree_leaf_t* leaf = inode_lock(ino);
    if (!leaf) return 0;

    // Refuse to unlink directories.
    if ((leaf->inode.i_mode & 0xF000) == EXT2_S_IFDIR) {
        inode_unlock(leaf);
        return 0;
    }

    // Remove directory entry first (takes parent inode lock internally).
    inode_unlock(leaf);  // drop briefly to avoid lock-order with parent
    if (!dir_remove_entry(parent_ino, basename)) return 0;
    leaf = inode_lock(ino);
    if (!leaf) return 0;

    // Decrement link count; only free inode/data when it reaches 0.
    if (leaf->inode.i_links_count > 0) leaf->inode.i_links_count--;
    if (leaf->inode.i_links_count == 0) {
        free_inode_blocks(&leaf->inode);
        leaf->inode.i_dtime = 1;
        inode_writeback(leaf);
        inode_unlock(leaf);
        free_inode_num(ino);
    } else {
        inode_writeback(leaf);
        inode_unlock(leaf);
    }

    return 1;
}

// ── ext2_rename ───────────────────────────────────────────────────────────
// Move/rename `src` to `dst`.  Returns 1 on success, 0 on failure.
// If `dst` already exists as a regular file it is removed first.
//
// Held under s_rename_lock for the entire operation so two concurrent
// renames cannot observe an intermediate state where the file briefly
// exists at both names (or both nowhere).  rename is a control-plane
// operation — taking a single global lock for it is what Linux does
// and is fine for any reasonable workload.
int ext2_rename(const char* src, const char* dst) {
    if (!s_mounted || !src || !dst) return 0;

    spin_lock(&s_rename_lock);

    uint32_t src_ino = path_to_inode(src);
    if (!src_ino) { spin_unlock(&s_rename_lock); return 0; }

    ext2_inode_t src_inode;
    if (!read_inode(src_ino, &src_inode)) { spin_unlock(&s_rename_lock); return 0; }

    uint8_t is_dir = ((src_inode.i_mode & 0xF000) == EXT2_S_IFDIR);

    // Resolve src parent/basename.
    char rn_src_parent[256];
    const char* src_base = path_split(src, rn_src_parent);
    if (!src_base || src_base[0] == '\0') { spin_unlock(&s_rename_lock); return 0; }
    uint32_t src_parent_ino = path_to_inode(rn_src_parent);
    if (!src_parent_ino) { spin_unlock(&s_rename_lock); return 0; }

    // Resolve dst parent/basename.
    char rn_dst_parent[256];
    const char* dst_base = path_split(dst, rn_dst_parent);
    if (!dst_base || dst_base[0] == '\0') { spin_unlock(&s_rename_lock); return 0; }
    uint32_t dst_parent_ino = path_to_inode(rn_dst_parent);
    if (!dst_parent_ino) { spin_unlock(&s_rename_lock); return 0; }

    // If dst exists as a regular file, unlink it under its inode lock.
    uint32_t dst_ino = path_to_inode(dst);
    if (dst_ino) {
        irtree_leaf_t* dleaf = inode_lock(dst_ino);
        if (!dleaf) { spin_unlock(&s_rename_lock); return 0; }
        if ((dleaf->inode.i_mode & 0xF000) == EXT2_S_IFDIR) {
            inode_unlock(dleaf);
            spin_unlock(&s_rename_lock);
            return 0;
        }
        // Drop briefly so dir_remove_entry can take the parent's lock
        // without our holding a child lock at the same time (avoids
        // potential parent↔child deadlock if a future codepath nests
        // them in the opposite order).
        inode_unlock(dleaf);
        dir_remove_entry(dst_parent_ino, dst_base);
        dleaf = inode_lock(dst_ino);
        if (dleaf) {
            free_inode_blocks(&dleaf->inode);
            dleaf->inode.i_links_count = 0;
            inode_writeback(dleaf);
            inode_unlock(dleaf);
        }
        free_inode_num(dst_ino);
    }

    // Add new directory entry pointing at the same inode.
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (!dir_add_entry(dst_parent_ino, dst_base, src_ino, ft)) {
        spin_unlock(&s_rename_lock);
        return 0;
    }

    // Remove old directory entry.
    dir_remove_entry(src_parent_ino, src_base);

    spin_unlock(&s_rename_lock);
    return 1;
}
