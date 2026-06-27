#include "ext2.h"
#include "ahci.h"
#include "kheap.h"
#include "common.h"
#include "smp.h"
#include "seqlock.h"
#include "rcu.h"
#include "checked.h"   // index_ok / ckd_add_u32: bounded-index + overflow-safe add
#include "dcache.h"    // Phase 7: cached path-component lookups +
                       // invalidation hooks at every mutation site

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
// The single fixed size of every block buffer: the bcache slots and all
// EXT2_SCRATCH allocations.  This is also the MAXIMUM ext2 block size the FS
// supports -- ext2_block_size_checked() rejects any superblock whose block
// size would exceed it, so a DMA read or memcpy of a full block can never
// overrun these buffers.  Keep the bcache slot dimension below in lockstep.
#define EXT2_BLOCK_SIZE_MAX 4096u
#define EXT2_SCRATCH_DECL(name) \
    uint8_t* name __attribute__((cleanup(ext2_scratch_free_))) = NULL
#define EXT2_SCRATCH_ALLOC(name) \
    do { name = (uint8_t*)kmalloc(EXT2_BLOCK_SIZE_MAX); if (!name) return 0; } while (0)
#define EXT2_SCRATCH_ALLOC_NEG1(name) \
    do { name = (uint8_t*)kmalloc(EXT2_BLOCK_SIZE_MAX); if (!name) return -1; } while (0)
#define EXT2_SCRATCH_ALLOC_VOID(name) \
    do { name = (uint8_t*)kmalloc(EXT2_BLOCK_SIZE_MAX); if (!name) return; } while (0)
// Convenience: declare + alloc in one step for cases where the function
// has already passed all early-error checks before the scratch is needed.
#define EXT2_SCRATCH(name)       EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC(name)
#define EXT2_SCRATCH_NEG1(name)  EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC_NEG1(name)
#define EXT2_SCRATCH_VOID(name)  EXT2_SCRATCH_DECL(name); EXT2_SCRATCH_ALLOC_VOID(name)

// ── Mount state ────────────────────────────────────────────────────────────

static uint32_t s_part_lba        = 0;
static uint32_t s_block_size      = 0;   // bytes per block (1024, 2048, or 4096)
static uint32_t s_sectors_per_blk = 0;   // block_size / 512

// Map an on-disk s_log_block_size to the block size in bytes, or 0 if the
// value is unsupported.  `log` comes straight from an untrusted superblock, so
// this is the validation gate: only 1024/2048/4096 (log 0/1/2) are accepted --
// every one of which is <= EXT2_BLOCK_SIZE_MAX (the fixed bcache slot / scratch
// size), so a full-block DMA read or memcpy can never overrun those buffers.
// log >= 3 would yield a block > 4096 (buffer overflow); log >= 32 is also a
// shift past the operand width (UB).  Pure -> unit-tested below.
static uint32_t ext2_block_size_checked(uint32_t log) {
    if (log > 2u) return 0;          // > 4096 unsupported; rejects the mount
    return 1024u << log;             // 1024 / 2048 / 4096
}
// PRIMITIVE (on-disk inode size validation): is `inode_size` (the untrusted
// superblock s_inode_size) usable for this block_size?  Require a power of two,
// at least the inode struct we copy (sizeof ext2_inode_t), and no larger than the
// block.  power-of-2 + <= block_size means inode_size DIVIDES block_size, so inodes
// tile a block without straddling: (local*inode_size) % block_size <= block_size -
// inode_size, hence off + sizeof(ext2_inode_t) <= block_size -- no OOB bcache-slot
// read (inode_load_into) or scratch write (inode_disk_write).  Pure -> unit-tested.
static inline int ext2_inode_size_valid(uint32_t inode_size, uint32_t block_size) {
    return inode_size >= sizeof(ext2_inode_t)
        && inode_size <= block_size
        && (inode_size & (inode_size - 1u)) == 0u;   // power of two
}
// PRIMITIVE (on-disk group geometry validation): a group's block bitmap and its
// inode bitmap each occupy exactly ONE filesystem block, so a group holds at
// most block_size*8 blocks and block_size*8 inodes.  s_blocks_per_group /
// s_inodes_per_group are untrusted u32 superblock fields; a forged value above
// block_size*8 makes the in-group bit index (rel % blocks_per_group, or the
// bitmap_find_free scan up to blocks_per_group) run past the fixed 4096-byte
// bitmap scratch -> heap OOB read/write in alloc_block / free_block /
// alloc_inode / free_inode_num.  block_size <= EXT2_BLOCK_SIZE_MAX (4096), so
// block_size*8 <= 32768 bits fits the scratch.  Also rejects zero (div-by-zero
// in the group math).  Pure -> unit-tested.
static inline int ext2_group_geom_valid(uint32_t blocks_per_grp,
                                        uint32_t inodes_per_grp,
                                        uint32_t block_size) {
    uint32_t cap = block_size * 8u;   // bits in a one-block bitmap
    return blocks_per_grp != 0 && inodes_per_grp != 0
        && blocks_per_grp <= cap && inodes_per_grp <= cap;
}
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
static uint32_t s_blocks_count    = 0;   // superblock's s_blocks_count (total FS blocks)
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

static uint8_t      s_bcache_data[BCACHE_NSETS][BCACHE_WAYS][EXT2_BLOCK_SIZE_MAX];
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
//
// Coherence rule: at most ONE way per set may hold a given blk.  Before
// picking a victim, invalidate any existing way that holds this blk —
// otherwise a fresh disk-write update can land in a second way while the
// old way keeps serving stale reads.  Invalidating even a pinned way is
// safe: the reader has already captured its data pointer and will finish
// its read against the (still-addressable) bytes; future lookups see
// tag=INVALID, miss, and re-fill from disk (which holds the fresh data).
static void bcache_fill(uint32_t blk, const uint8_t* data, uint32_t len) {
    uint32_t set = bcache_set_of(blk);

    // Evict any way currently tagged with `blk` so the new data isn't
    // shadowed by a stale copy in another way.
    for (uint32_t w = 0; w < BCACHE_WAYS; w++) {
        bcache_way_t* m = &s_bcache_meta[set][w];
        if (__atomic_load_n(&m->tag, __ATOMIC_ACQUIRE) == blk)
            __atomic_store_n(&m->tag, BCACHE_INVALID, __ATOMIC_RELEASE);
    }

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
// ── Physical block-number validation ──────────────────────────────────────
// Every block number that becomes a device LBA (lba = s_part_lba + blk*spb)
// must be proven in-range FIRST.  i_block[] entries, indirect-block entries and
// BGD fields are all untrusted on-disk data; a wild value would DMA a sector
// outside the filesystem -- a cross-partition / off-device read, or an OOB
// write on inode writeback.  Valid physical blocks are [s_first_data_blk,
// s_blocks_count).  The math is factored into pure helpers so it is unit-tested
// (ext2_block_valid_selftest) independent of any mounted image.
// PRIMITIVE (on-disk block index + run, category D -> index_ok / ckd_add_u32).
static inline int ext2_block_in_range(uint32_t blk, uint32_t first, uint32_t count) {
    return blk >= first && index_ok(blk, count);   // [first, count), half-open upper
}
// Are blocks [start, start+run) ALL valid?  Overflow-safe: start+run can wrap.
static inline int ext2_run_in_range(uint32_t start, uint32_t run,
                                    uint32_t first, uint32_t count) {
    if (run == 0)      return 1;           // empty run is vacuously valid
    if (start < first) return 0;           // below the first data block
    uint32_t end;                          // end = start + run, overflow-safe
    if (!ckd_add_u32(start, run, &end)) return 0;   // wrap -> reject
    return end <= count;                   // [start, start+run) within the FS
}
static inline int ext2_block_valid(uint32_t blk) {
    return ext2_block_in_range(blk, s_first_data_blk, s_blocks_count);
}
static inline int ext2_run_valid(uint32_t start, uint32_t run) {
    return ext2_run_in_range(start, run, s_first_data_blk, s_blocks_count);
}
// PRIMITIVE (on-disk directory entry bounds).  A dirent at byte `off` carries an
// untrusted on-disk rec_len (uint16) + name_len (uint8).  Before reusing/splitting
// it (dir_add_entry) or comparing its name (dir_remove_entry), require it to be
// well-formed within the blk_bytes-sized block: rec_len keeps the WHOLE entry in
// the block (off + rec_len <= blk_bytes), and the aligned header+name fits the
// entry (align4(8 + name_len) <= rec_len).  Without this a corrupt rec_len/name_len
// makes `rec_len - actual_len` UNDERFLOW (huge slack) so the split writes
// `dae_buf + off + actual_len` PAST the 4096-byte heap block = heap OOB write.
// Pure -> unit-tested (ext2_dirent_in_block_selftest).
static inline int ext2_dirent_in_block(uint32_t off, uint32_t rec_len,
                                       uint32_t name_len, uint32_t blk_bytes) {
    uint32_t actual = 8u + name_len;
    if (actual & 3u) actual = (actual | 3u) + 1u;      // align to 4 (same as the walk)
    if ((uint64_t)off + rec_len > blk_bytes) return 0; // entry runs past the block
    return actual <= rec_len;                          // header+name fits its rec_len
}
// PRIMITIVE (on-disk block number -> 64-bit device LBA).  blk is validated by
// ext2_block_valid, but the LBA MUST be formed in 64-bit: blk*spb can exceed
// 2^32 for a large filesystem or a crafted s_blocks_count, and a 32-bit product
// WRAPS to a wrong sector (silent OOB read/write within the device).  No
// overflow check is needed -- blk < 2^32 and spb <= 8, so the product is < 2^35
// and the +part_lba sum < 2^36, all within u64.  Factored pure for unit testing.
static inline uint64_t ext2_blk_lba(uint32_t part_lba, uint32_t blk, uint32_t spb) {
    return (uint64_t)part_lba + (uint64_t)blk * spb;
}
static inline uint64_t ext2_blk_to_lba(uint32_t blk) {
    return ext2_blk_lba(s_part_lba, blk, s_sectors_per_blk);
}

static bcache_ref_t bcache_get(uint32_t blk, uint8_t* scratch) {
    bcache_ref_t r = { NULL, blk, 0, 0, 0 };
    // Reject any block outside the filesystem before it can become an LBA or
    // pollute the cache (untrusted i_block[]/indirect/BGD source).  r.data stays
    // NULL, which every caller already treats as an I/O error.
    if (!ext2_block_valid(blk)) return r;
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
    uint64_t lba = ext2_blk_to_lba(blk);   // 64-bit LBA: blk*spb must not wrap
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
//   - Writers use inode_lock / inode_unlock.  inode_lock takes ONLY the
//     leaf's internal write_lock (serialising writers of the same inode);
//     it does NOT bump the sequence odd, so it may be held across blocking
//     disk I/O without making readers spin.  The sequence is bumped odd
//     only by inode_pub_begin / inode_pub_end around the brief in-memory
//     publish of leaf->inode (SCAN #13: holding the sequence odd across
//     I/O deadlocked a preempt-disabled irtree_get reader against the
//     sleeping writer).  Two CPUs writing different inodes never contend.
//   - The L0 / L1 pointer slots are publish-once stores; lookup is a
//     plain acquire-load with no atomics on the hot path.  Allocation
//     of a fresh leaf the first time we touch a given inode is
//     serialised by `s_irtree_alloc_lock`, which only fires once per
//     unique inode for the lifetime of the kernel.
//
// The leaf's `inode` field doubles as the in-memory authoritative copy
// of the inode for read-modify-write sequences: callers do
// `leaf = inode_lock(ino)` (which fault-loads the disk image into the
// leaf if needed), do their disk I/O, then publish the final bytes with
// inode_pub_begin(); leaf->inode = ...; inode_pub_end(); before
// inode_writeback() persists them and inode_unlock() drops the write_lock.
// The seqcount bump on the publish is what lets concurrent irtree_get
// readers notice the change without taking any lock.

#define IRTREE_BITS  8
#define IRTREE_SIZE  (1u << IRTREE_BITS)   // 256
#define IRTREE_MASK  (IRTREE_SIZE - 1u)

typedef struct irtree_leaf_t {
    seqlock_t    seq;             // per-inode seqlock; readers never block
    ext2_inode_t inode;
    uint32_t     ino;
    uint8_t      valid;

    // Phase 7E: refcount + activity timestamp for eviction.  Bumped
    // by inode_lock (writer) and by the implicit hold during
    // irtree_get's rcu_read_lock section.  last_used_ns is updated
    // on every access (relaxed store — pure hint for the shrinker).
    volatile uint32_t refcount;
    volatile uint64_t last_used_ns;

    // Phase 7E: RCU callback head for async free under
    // SLAB_TYPESAFE_BY_RCU semantics.  Memory stays irtree_leaf_t-
    // typed during the grace period so concurrent irtree_get
    // readers are safe to dereference.
    rcu_head_t   rcu_head;
} irtree_leaf_t;

typedef struct {
    irtree_leaf_t* leaves[IRTREE_SIZE];
} irtree_l1_t;

static irtree_l1_t* s_irtree[IRTREE_SIZE];  // L0: 256 pointers
// Allocation + eviction lock.  Taken briefly on first-touch alloc
// and by the shrinker when evicting.
static spinlock_t   s_irtree_alloc_lock = SPINLOCK_INIT;

// Phase 7E: dedicated SLAB_TYPESAFE_BY_RCU cache for leaves.  After
// an eviction, the page memory remains irtree_leaf_t-typed for an
// RCU grace period so readers mid-irtree_get stay safe even if they
// already loaded the pointer.  The seqlock + ino check detects
// instance reuse (fresh alloc in the same slot).
static slab_cache_t s_irtree_leaf_cache;
static uint8_t       s_irtree_cache_inited = 0;

extern uint64_t tsc_read_ns(void);

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
// Lazy-init the typesafe leaf cache on first use.  Called under
// s_irtree_alloc_lock so we don't race two CPUs initialising the
// same cache struct.
static void irtree_cache_init_once_locked(void) {
    if (s_irtree_cache_inited) return;
    pmm_slab_cache_init(&s_irtree_leaf_cache, sizeof(irtree_leaf_t),
                        SLAB_TYPESAFE_BY_RCU);
    s_irtree_cache_inited = 1;
}

static irtree_leaf_t* irtree_alloc_with_ref(uint32_t ino) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;

    spin_lock(&s_irtree_alloc_lock);
    irtree_cache_init_once_locked();

    irtree_l1_t* l1tab = s_irtree[l0];
    if (!l1tab) {
        l1tab = kmalloc(sizeof(irtree_l1_t));
        if (!l1tab) { spin_unlock(&s_irtree_alloc_lock); return NULL; }
        for (uint32_t i = 0; i < IRTREE_SIZE; i++) l1tab->leaves[i] = NULL;
        __atomic_store_n(&s_irtree[l0], l1tab, __ATOMIC_RELEASE);
    }
    irtree_leaf_t* leaf = l1tab->leaves[l1];
    if (leaf) {
        // Already present (another CPU raced us here).  Bump its
        // refcount and return — the caller's rcu_read_lock kept it
        // alive against any concurrent eviction that might have
        // been in flight before we grabbed s_irtree_alloc_lock.
        __atomic_fetch_add(&leaf->refcount, 1u, __ATOMIC_ACQ_REL);
        __atomic_store_n(&leaf->last_used_ns, tsc_read_ns(), __ATOMIC_RELAXED);
        spin_unlock(&s_irtree_alloc_lock);
        return leaf;
    }

    // Fresh allocation from the typesafe slab.
    leaf = (irtree_leaf_t*)pmm_slab_alloc(&s_irtree_leaf_cache);
    if (!leaf) { spin_unlock(&s_irtree_alloc_lock); return NULL; }
    seqlock_init(&leaf->seq);
    leaf->ino          = ino;
    leaf->valid        = 0;
    leaf->refcount     = 1;     // caller gets one reference
    leaf->last_used_ns = tsc_read_ns();
    __atomic_store_n(&l1tab->leaves[l1], leaf, __ATOMIC_RELEASE);

    spin_unlock(&s_irtree_alloc_lock);
    return leaf;
}

// Forward decl: read inode from disk into a caller-owned buffer.
// No locking — safe to call without holding anything.
static uint8_t inode_load_into(uint32_t ino, ext2_inode_t* out);

// Public per-inode write-side lock.  Returns the leaf with its per-inode
// write_lock (the seqlock's embedded spinlock) held to SERIALISE writers --
// but with the sequence counter EVEN, NOT bumped to the odd "write in
// progress" state.
//
// Critical design rule (SCAN #13 fix): the seqlock SEQUENCE is never held odd
// across blocking disk I/O.  A writer may sleep arbitrarily long inside
// write_block/inode_writeback (AHCI submit + completion wait); if it held the
// sequence odd across that sleep, every concurrent irtree_get reader would spin
// in seq_begin's cpu_relax -- and irtree_get spins with rcu_read_lock held
// (== preempt_disable), so a reader on the sleeping writer's CPU can never be
// preempted to let the writer run.  That is a hard deadlock (a preempt-disabled
// reader starves the runnable task that would complete the writer's I/O), which
// the AHCI completion self-heal cannot break.  So inode_lock holds ONLY the
// write_lock across the I/O (a plain spinlock -- preemptible, so writer-vs-writer
// contention self-heals); the sequence is bumped odd ONLY by inode_pub_begin /
// inode_pub_end around the brief IN-MEMORY publish of leaf->inode (no I/O there).
// The seqlock protects exactly the 128-byte leaf->inode snapshot irtree_get
// copies -- it never protected directory data blocks (those go through bcache),
// so moving the I/O outside the sequence weakens no guarantee: a reader gets a
// consistent pre- or post-publish inode, exactly as before.
//
// Benign race: two CPUs may load the same inode concurrently; both
// publish identical bytes.  Last-writer-wins is correct.
static irtree_leaf_t* inode_lock(uint32_t ino) {
    // Phase 7E: lookup + refcount bump atomic w.r.t. eviction.
    // rcu_read_lock pins any leaf we find against concurrent reclaim.
    // On hit, we bump refcount before dropping rcu.  On miss, we
    // fall through to irtree_alloc_with_ref which takes the
    // alloc-lock and installs a new leaf (also refcounted).
    rcu_read_lock();
    irtree_leaf_t* leaf = irtree_lookup(ino);
    if (leaf && leaf->ino == ino) {
        __atomic_fetch_add(&leaf->refcount, 1u, __ATOMIC_ACQ_REL);
        __atomic_store_n(&leaf->last_used_ns, tsc_read_ns(), __ATOMIC_RELAXED);
        rcu_read_unlock();
    } else {
        rcu_read_unlock();
        leaf = irtree_alloc_with_ref(ino);
        if (!leaf) return NULL;
    }

    if (!leaf->valid) {
        ext2_inode_t tmp;
        if (!inode_load_into(ino, &tmp)) {
            __atomic_fetch_sub(&leaf->refcount, 1u, __ATOMIC_ACQ_REL);
            return NULL;
        }
        seq_write_begin(&leaf->seq);
        if (!leaf->valid) {              // re-check under write lock
            leaf->inode = tmp;
            leaf->ino   = ino;
            leaf->valid = 1;
        }
        seq_write_end(&leaf->seq);
    }

    // Serialise writers WITHOUT bumping the sequence odd: hold only the
    // embedded write_lock across the caller's (possibly I/O-bearing) critical
    // section.  Readers see a consistent EVEN sequence and never spin here.
    spin_lock(&leaf->seq.write_lock);
    return leaf;
}

static void inode_unlock(irtree_leaf_t* leaf) {
    if (!leaf) return;
    spin_unlock(&leaf->seq.write_lock);
    __atomic_fetch_sub(&leaf->refcount, 1u, __ATOMIC_ACQ_REL);
}

// Publish in-memory changes to leaf->inode (and leaf->valid) atomically w.r.t.
// seqlock readers.  The caller MUST already hold the write side via inode_lock
// (write_lock held), so these bump ONLY the sequence (the _unlocked variants --
// no nested spinlock).  NO blocking I/O may run between begin and end: that is
// the whole point (readers spin in seq_begin only for this nanosecond-scale
// window, never across the writer's disk I/O).
static inline void inode_pub_begin(irtree_leaf_t* leaf) {
    seq_write_begin_unlocked(&leaf->seq);
}
static inline void inode_pub_end(irtree_leaf_t* leaf) {
    seq_write_end_unlocked(&leaf->seq);
}

// Brief-section reader: copy the cached inode out under a seqlock retry
// loop.  No atomics on a consistent read.  Loops on the rare collision
// where a writer is mid-update.
//
// Phase 7E: wrapped in rcu_read_lock so the leaf memory stays
// irtree_leaf_t-typed even if a concurrent shrinker evicts the slot.
// The leaf->ino check inside the retry loop detects slab slot reuse
// across a grace period.
static uint8_t irtree_get(uint32_t ino, ext2_inode_t* out) {
    rcu_read_lock();
    irtree_leaf_t* leaf = irtree_lookup(ino);
    if (!leaf) { rcu_read_unlock(); return 0; }
    uint32_t s;
    uint8_t valid;
    do {
        s = seq_begin(&leaf->seq);
        valid = leaf->valid && leaf->ino == ino;
        if (valid) *out = leaf->inode;
    } while (seq_retry(&leaf->seq, s));
    // Bump last_used_ns even on a hit — keeps the leaf out of the
    // shrinker's "cold" set as long as something's reading it.
    // Relaxed store; pure hint.
    if (valid) __atomic_store_n(&leaf->last_used_ns, tsc_read_ns(),
                                  __ATOMIC_RELAXED);
    rcu_read_unlock();
    return valid;
}

// ── Phase 7E: inode cache shrinker ─────────────────────────────────────
//
// Walks every (l0, l1) slot, evicts leaves with refcount == 0 older
// than `idle_ns_cutoff`, up to `max` evictions per call.
//
// Eviction sequence:
//   1. Under s_irtree_alloc_lock, re-check refcount == 0.
//   2. atomic_store NULL into l1tab->leaves[l1] — future lookups miss.
//   3. call_rcu_head(&leaf->rcu_head, irtree_leaf_free_cb, leaf) —
//      defer the slab free until every concurrent irtree_get reader
//      has passed through an RCU quiescent state.
//
// Readers mid-irtree_get who already loaded the leaf pointer stay
// safe because:
//   - They're inside rcu_read_lock; the callback can't fire until
//     they rcu_read_unlock.
//   - SLAB_TYPESAFE_BY_RCU keeps the memory irtree_leaf_t-typed for
//     the grace period, so struct-field access doesn't UAF.
//   - The leaf->ino check inside the seqlock loop catches instance
//     reuse if the slab slot gets recycled by another inode.

static void irtree_leaf_free_cb(void* p) {
    pmm_slab_free(p);
}

// Returns number of leaves evicted.  `idle_ns_cutoff` = "if
// last_used_ns < now - cutoff, candidate for eviction".  `max`
// caps the scan to bounded work per call.
uint32_t irtree_shrink(uint32_t max, uint64_t idle_ns_cutoff) {
    if (!s_irtree_cache_inited || max == 0) return 0;
    uint64_t now = tsc_read_ns();
    uint32_t evicted = 0;

    for (uint32_t l0 = 0; l0 < IRTREE_SIZE && evicted < max; l0++) {
        irtree_l1_t* l1tab = __atomic_load_n(&s_irtree[l0], __ATOMIC_ACQUIRE);
        if (!l1tab) continue;
        for (uint32_t l1 = 0; l1 < IRTREE_SIZE && evicted < max; l1++) {
            irtree_leaf_t* leaf = __atomic_load_n(&l1tab->leaves[l1],
                                                    __ATOMIC_ACQUIRE);
            if (!leaf) continue;

            // Quick unlocked check — spares the lock acquire when
            // the leaf is hot.  A racy race (leaf gets touched between
            // the check and the lock) just means we bail inside the
            // locked block.
            if (__atomic_load_n(&leaf->refcount, __ATOMIC_ACQUIRE) != 0)
                continue;
            uint64_t last = __atomic_load_n(&leaf->last_used_ns,
                                              __ATOMIC_RELAXED);
            if (now - last < idle_ns_cutoff) continue;

            // Evict under the alloc lock (serialises vs concurrent
            // irtree_alloc_with_ref on the same slot).
            spin_lock(&s_irtree_alloc_lock);
            // Re-check: refcount could have been bumped, leaf could
            // have been evicted by another shrinker, etc.
            if (__atomic_load_n(&l1tab->leaves[l1], __ATOMIC_RELAXED) != leaf
                || __atomic_load_n(&leaf->refcount, __ATOMIC_ACQUIRE) != 0) {
                spin_unlock(&s_irtree_alloc_lock);
                continue;
            }
            // Unpublish: future irtree_lookup returns NULL for this ino.
            __atomic_store_n(&l1tab->leaves[l1], NULL, __ATOMIC_RELEASE);
            spin_unlock(&s_irtree_alloc_lock);
            call_rcu_head(&leaf->rcu_head, irtree_leaf_free_cb, leaf);
            evicted++;
        }
    }
    return evicted;
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
    if (!ext2_block_valid(blk)) return 0;   // never write outside the filesystem
    uint64_t lba = ext2_blk_to_lba(blk);   // 64-bit LBA: blk*spb must not wrap
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
static uint8_t inode_load_into(uint32_t ino, ext2_inode_t* out) {
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
    __builtin_memcpy(out, r.data + off, sizeof(ext2_inode_t));
    bcache_put(&r);
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
    uint8_t sb_buf[1024];
    if (!ahci_read(part_lba + 2, sb_buf, 2)) return 0;

    ext2_superblock_t* sb = (ext2_superblock_t*)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) return 0;

    // Validate the block size from the untrusted superblock before any block
    // read: an out-of-range s_log_block_size would make s_block_size exceed the
    // fixed 4096-byte bcache slots / DMA scratch (heap+BSS overflow) or shift UB.
    s_block_size      = ext2_block_size_checked(sb->s_log_block_size);
    if (s_block_size == 0) return 0;   // unsupported block size -> refuse mount
    s_sectors_per_blk = s_block_size / 512;
    s_inodes_per_grp  = sb->s_inodes_per_group;
    s_blocks_per_grp  = sb->s_blocks_per_group;
    s_first_data_blk  = sb->s_first_data_block;
    s_blocks_count    = sb->s_blocks_count;
    s_inode_size      = (sb->s_rev_level >= 1 && sb->s_inode_size > 0)
                        ? sb->s_inode_size : 128;

    // Reject a degenerate superblock (all fields untrusted on-disk data): a zero
    // total block count would make every block-range check fail.  s_block_size
    // was already validated by ext2_block_size_checked.
    if (s_blocks_count == 0) return 0;
    // The per-group block and inode bitmaps each occupy one block, so blocks-
    // and inodes-per-group must each be in [1, block_size*8].  A larger forged
    // value would overrun the fixed 4096-byte bitmap scratch in alloc/free ->
    // heap OOB read/write.  Refuse the mount.
    if (!ext2_group_geom_valid(s_blocks_per_grp, s_inodes_per_grp, s_block_size))
        return 0;

    // s_inode_size is untrusted (u16 on disk): a value that is not a power of two,
    // smaller than the inode struct we copy, or larger than the block makes an inode
    // straddle the bcache slot -> OOB read in inode_load_into / OOB write in
    // inode_disk_write.  Refuse the mount.
    if (!ext2_inode_size_valid(s_inode_size, s_block_size)) return 0;

    // Number of block groups.
    s_num_groups = (s_blocks_count + s_blocks_per_grp - 1) / s_blocks_per_grp;

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

// ── inode_build_run ────────────────────────────────────────────────────────
// Resolve a run of consecutive physical blocks starting at logical blk_idx.
// Unlike calling inode_get_block per block, indirect metadata is pinned once
// for the whole probe: no repeated bcache_get/put/kmalloc per block.
//
// Returns the run length (>= 1) and sets *phys_start to the first physical
// block number.  *phys_start == 0 means the first block is sparse/unallocated;
// the caller should zero-fill rather than DMA.
//
// The run is bounded by max_run AND by the boundary of the current indirect
// level (direct 0-11, singly-indirect 12..12+N-1, doubly-indirect …).  The
// outer read loop issues additional calls for the next region if needed.
//
// On kmalloc failure for scratch, falls back to inode_get_block (correct but
// slower).  The fast path is always correct; the fallback is just less fast.
static uint32_t inode_build_run(const ext2_inode_t* ino, uint32_t blk_idx,
                                  uint32_t max_run, uint32_t* phys_start)
{
    if (max_run == 0) { *phys_start = 0; return 1; }
    uint32_t addrs_per_blk = s_block_size / 4;

    // ── Direct range (logical blocks 0–11) ────────────────────────────────
    if (blk_idx < 12) {
        uint32_t first = ino->i_block[blk_idx];
        *phys_start = first;
        if (!first || max_run == 1) return 1;
        // Extend within the direct window; stop at 12 (boundary with indirect).
        uint32_t direct_lim = 12u - blk_idx;
        if (direct_lim > max_run) direct_lim = max_run;
        uint32_t run = 1;
        for (; run < direct_lim; run++)
            if (ino->i_block[blk_idx + run] != first + run) break;
        return run;
    }

    // ── Singly-indirect range (logical 12 .. 12+addrs_per_blk-1) ─────────
    uint32_t si_rel = blk_idx - 12u;
    if (si_rel < addrs_per_blk) {
        uint32_t ind_phys = ino->i_block[12];
        if (!ind_phys) { *phys_start = 0; return 1; }

        // Pin the indirect block once; read all block pointers from it in a
        // single pass with no further bcache_get per block.
        uint8_t* scratch = (uint8_t*)kmalloc(4096);
        if (!scratch) {
            // OOM fallback: resolve the single starting block via the slow path.
            *phys_start = inode_get_block(ino, blk_idx);
            return 1;
        }
        bcache_ref_t r = bcache_get(ind_phys, scratch);
        if (!r.data) { kfree(scratch); *phys_start = 0; return 1; }

        const uint32_t* arr = (const uint32_t*)r.data;
        uint32_t first = arr[si_rel];
        *phys_start = first;
        uint32_t run = 1;
        if (first && max_run > 1) {
            uint32_t lim = addrs_per_blk - si_rel;
            if (lim > max_run) lim = max_run;
            for (; run < lim; run++)
                if (arr[si_rel + run] != first + run) break;
        }
        bcache_put(&r);
        kfree(scratch);
        return run;
    }

    // ── Doubly-indirect range ─────────────────────────────────────────────
    uint32_t di_rel = blk_idx - 12u - addrs_per_blk;
    if (di_rel < addrs_per_blk * addrs_per_blk) {
        uint32_t l1_idx    = di_rel / addrs_per_blk;
        uint32_t l2_rel    = di_rel % addrs_per_blk;
        uint32_t dind_phys = ino->i_block[13];
        if (!dind_phys) { *phys_start = 0; return 1; }

        // Read L1 (doubly-indirect) block to find the L2 block number.
        uint8_t* s1 = (uint8_t*)kmalloc(4096);
        if (!s1) { *phys_start = inode_get_block(ino, blk_idx); return 1; }
        bcache_ref_t r1 = bcache_get(dind_phys, s1);
        if (!r1.data) { kfree(s1); *phys_start = 0; return 1; }
        uint32_t l2_phys = ((const uint32_t*)r1.data)[l1_idx];
        bcache_put(&r1);
        kfree(s1);
        if (!l2_phys) { *phys_start = 0; return 1; }

        // Pin the L2 block once; probe the full run from it.
        uint8_t* s2 = (uint8_t*)kmalloc(4096);
        if (!s2) { *phys_start = inode_get_block(ino, blk_idx); return 1; }
        bcache_ref_t r2 = bcache_get(l2_phys, s2);
        if (!r2.data) { kfree(s2); *phys_start = 0; return 1; }

        const uint32_t* arr = (const uint32_t*)r2.data;
        uint32_t first = arr[l2_rel];
        *phys_start = first;
        uint32_t run = 1;
        if (first && max_run > 1) {
            uint32_t lim = addrs_per_blk - l2_rel;
            if (lim > max_run) lim = max_run;
            for (; run < lim; run++)
                if (arr[l2_rel + run] != first + run) break;
        }
        bcache_put(&r2);
        kfree(s2);
        return run;
    }

    // Triple-indirect not implemented — fall back to per-block resolution.
    *phys_start = inode_get_block(ino, blk_idx);
    return 1;
}

// ── Directory lookup ───────────────────────────────────────────────────────

// Defined below (before ext2_readdir); forward-declared so dir_lookup can use
// the same dirent-name bounds clamp.
uint32_t ext2_dirent_namelen_clamp(uint32_t off, uint32_t name_len, uint32_t blk_bytes);

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
            // Clamp guard: a corrupt dirent near the block tail could claim a
            // name that runs past the 4096-byte bcache slot; only compare when
            // the name actually fits in the block (same boundary as readdir).
            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                ext2_dirent_namelen_clamp(off, de->name_len, blk_bytes) >= name_len &&
                kmemeq(de->name, name, name_len)) {
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

    // Partial-block scratch — only allocated for non-block-aligned reads
    // (first/last partial block of a seek'd read).  The fast path below
    // bypasses the bcache entirely for full-block aligned reads.
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

        // ── Fast path: block-aligned, full-block reads ────────────────────
        // inode_build_run pins the appropriate indirect block once and probes
        // all consecutive physical blocks in a tight loop — no per-block
        // bcache_get/kmalloc.  Then a single ahci_read DMA covers the whole
        // run, regardless of whether blocks are consecutive or not (a non-
        // consecutive boundary just shortens the run; the outer loop retries).
        // Even a run of 1 block uses direct DMA instead of going through
        // bcache, eliminating pin/seqlock overhead on the cold path.
        if (off_in_blk == 0 && to_copy == s_block_size) {
            uint32_t max_run = (uint32_t)((len - total) / s_block_size);
            uint32_t file_blks_left = (remain_file + s_block_size - 1) / s_block_size;
            if (max_run > file_blks_left) max_run = file_blks_left;
            if (max_run == 0) max_run = 1;
            uint32_t dma_cap = AHCI_DMA_SECTORS / s_sectors_per_blk;
            if (max_run > dma_cap) max_run = dma_cap;

            uint32_t phys_blk;
            uint32_t run   = inode_build_run(&fd->inode, blk_idx, max_run, &phys_blk);
            uint32_t bytes = run * s_block_size;

            if (!phys_blk) {
                // Sparse block(s): zero-fill without any disk I/O.
                __builtin_memset(dst + total, 0, bytes);
            } else {
                // Untrusted block ptr from inode/indirect -> never DMA a run
                // that escapes the filesystem.
                if (!ext2_run_valid(phys_blk, run)) return -1;
                uint64_t lba     = ext2_blk_to_lba(phys_blk);   // 64-bit LBA: must not wrap
                uint32_t sectors = run * s_sectors_per_blk;
                uint8_t* dest    = dst + total;
                // User-space buffer → scatter-gather zero-copy via HHDM page
                // resolution.  Kernel buffer (ELF loader, etc.) → plain
                // ahci_read into the HHDM-mapped kmalloc'd staging buffer.
                uint8_t is_user = ((uint64_t)dest < HHDM_OFFSET);
                if (is_user) {
                    if (!ahci_read_user(lba, dest, sectors)) return -1;
                } else {
                    if (!ahci_read(lba, dest, sectors)) return -1;
                }
                // Warm the SHARED block cache from the just-DMA'd data so a
                // second exec of the same binary is a pure cache hit -- but ONLY
                // when the source is a trusted KERNEL buffer (ELF loader / demand
                // paging, is_user==0).  For a user destination `dest` is unpinned
                // user memory (ahci_read_user unpinned it) that a sibling thread
                // can overwrite between the DMA and this memcpy: warming from it
                // would poison the shared cross-process cache slot tagged
                // phys_blk with attacker bytes (and a racing munmap would fault
                // here in kernel mode).  The user-read path simply forgoes the
                // opportunistic warm; a later read re-fills from disk.  (bcache_
                // fill silently drops updates for currently-pinned slots.)
                if (!is_user)
                    for (uint32_t i = 0; i < run; i++)
                        bcache_fill(phys_blk + i,
                                    dest + (uint64_t)i * s_block_size,
                                    s_block_size);
            }
            // file_blks_left rounds UP, so a run covering the file's
            // final block DMA'd that block's tail slack into dst too.
            // The buffer is big enough (max_run is bounded by len), but
            // the byte COUNT must clamp at EOF: without this a
            // 7447-byte file read back as 8×1024 = 8192 bytes, and
            // sway's "config file changed during reading" guard fired
            // on the phantom tail.
            if (bytes > remain_file) bytes = remain_file;
            total       += bytes;
            fd->cur_pos += bytes;
            continue;
        }

        // ── Slow path: partial block ──────────────────────────────────────
        // Handles the first/last partial block of a mis-aligned or
        // end-of-file read.  Go through bcache so future aligned reads of
        // the same block get a zero-copy cache hit.
        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            for (uint32_t i = 0; i < to_copy; i++) dst[total + i] = 0;
            total       += to_copy;
            fd->cur_pos += to_copy;
            continue;
        }
        if (!rd_buf) EXT2_SCRATCH_ALLOC_NEG1(rd_buf);
        bcache_ref_t r = bcache_get(blk, rd_buf);
        if (!r.data) return -1;
        __builtin_memcpy(dst + total, r.data + off_in_blk, to_copy);
        bcache_put(&r);
        total       += to_copy;
        fd->cur_pos += to_copy;
    }

    return (int64_t)total;
}

// Positional read: like ext2_vfs_read but at an explicit offset, no cur_pos change.
// Thread-safe for concurrent pread calls on shared vfs_file_t (page fault path).
static int64_t ext2_vfs_pread(vfs_file_t* self, void* buf, uint64_t len, uint64_t offset) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd) return -1;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;
    uint32_t cur_pos = (uint32_t)offset;   // local — never written back to fd

    EXT2_SCRATCH_DECL(rd_buf);

    while (total < len) {
        if (cur_pos >= fd->file_size) break;

        uint32_t blk_idx     = cur_pos / s_block_size;
        uint32_t off_in_blk  = cur_pos % s_block_size;
        uint32_t remain_file = fd->file_size - cur_pos;
        uint32_t remain_blk  = s_block_size - off_in_blk;

        uint32_t to_copy = (uint32_t)(len - total);
        if (to_copy > remain_blk)  to_copy = remain_blk;
        if (to_copy > remain_file) to_copy = remain_file;

        if (off_in_blk == 0 && to_copy == s_block_size) {
            uint32_t max_run = (uint32_t)((len - total) / s_block_size);
            uint32_t file_blks_left = (remain_file + s_block_size - 1) / s_block_size;
            if (max_run > file_blks_left) max_run = file_blks_left;
            if (max_run == 0) max_run = 1;
            uint32_t dma_cap = AHCI_DMA_SECTORS / s_sectors_per_blk;
            if (max_run > dma_cap) max_run = dma_cap;

            uint32_t phys_blk;
            uint32_t run   = inode_build_run(&fd->inode, blk_idx, max_run, &phys_blk);
            uint32_t bytes = run * s_block_size;

            if (!phys_blk) {
                __builtin_memset(dst + total, 0, bytes);
            } else {
                // Untrusted block ptr -> bound the run to the filesystem.
                if (!ext2_run_valid(phys_blk, run)) return -1;
                uint64_t lba     = ext2_blk_to_lba(phys_blk);   // 64-bit LBA: must not wrap
                uint32_t sectors = run * s_sectors_per_blk;
                uint8_t* dest    = dst + total;
                uint8_t is_user = ((uint64_t)dest < HHDM_OFFSET);
                if (is_user) {
                    if (!ahci_read_user(lba, dest, sectors)) return -1;
                } else {
                    if (!ahci_read(lba, dest, sectors)) return -1;
                }
                // Warm the SHARED cache ONLY from a trusted kernel source -- a
                // user `dest` is unpinned user memory a sibling thread can
                // overwrite between the DMA and this memcpy, which would poison
                // the cross-process cache slot tagged phys_blk (same as
                // ext2_vfs_read above).
                if (!is_user)
                    for (uint32_t i = 0; i < run; i++)
                        bcache_fill(phys_blk + i,
                                    dest + (uint64_t)i * s_block_size,
                                    s_block_size);
            }
            total   += bytes;
            cur_pos += bytes;
            continue;
        }

        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            for (uint32_t i = 0; i < to_copy; i++) dst[total + i] = 0;
            total   += to_copy;
            cur_pos += to_copy;
            continue;
        }
        if (!rd_buf) EXT2_SCRATCH_ALLOC_NEG1(rd_buf);
        bcache_ref_t r = bcache_get(blk, rd_buf);
        if (!r.data) return -1;
        __builtin_memcpy(dst + total, r.data + off_in_blk, to_copy);
        bcache_put(&r);
        total   += to_copy;
        cur_pos += to_copy;
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
        inode_pub_begin(leaf);
        leaf->inode = fd->inode;
        inode_pub_end(leaf);
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

// ── ext2_open_by_ino (internal) ────────────────────────────────────────────
// Build a vfs_file_t for an already-resolved inode number.  `path` is stored
// in f->path for fstat/ftruncate; pass NULL if unavailable (f->path will be
// empty).
//
// Change 1 (double-lookup fix): callers that already hold the inode number
// from a prior ext2_lookup_path() call use this directly, avoiding a second
// full path walk.
//
// Change 4 (indirect-block prefetch): after reading the inode, eagerly warm
// the bcache for the singly- and doubly-indirect block maps.  One AHCI read
// at open time amortises the cold-miss cost over every inode_build_run() call
// during the file's lifetime — the first indirect-range block lookup is
// guaranteed to be a cache hit.
static vfs_file_t* ext2_open_by_ino(uint32_t ino, const char* path) {
    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return NULL;
    if (!(inode.i_mode & EXT2_S_IFREG)) return NULL;

    // Prefetch indirect block maps into bcache.  A single scratch buffer
    // covers both reads; we just pin-and-release to trigger the fill.
    // Skipped silently on OOM — the warm path will still work, just with
    // one extra cold miss.
    {
        uint8_t* pre = (uint8_t*)kmalloc(4096);
        if (pre) {
            if (inode.i_block[12]) {
                bcache_ref_t r = bcache_get(inode.i_block[12], pre);
                bcache_put(&r);
            }
            if (inode.i_block[13]) {
                bcache_ref_t r = bcache_get(inode.i_block[13], pre);
                bcache_put(&r);
            }
            kfree(pre);
        }
    }

    ext2_fd_t* fd = (ext2_fd_t*)kmalloc(sizeof(ext2_fd_t));
    if (!fd) return NULL;
    fd->ino       = ino;
    fd->cur_pos   = 0;
    fd->file_size = inode.i_size;
    fd->inode     = inode;

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(fd); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));

    f->read            = ext2_vfs_read;
    f->write           = ext2_vfs_write;
    f->seek            = ext2_vfs_seek;
    f->pread           = ext2_vfs_pread;
    f->close           = ext2_vfs_close;
    f->poll            = NULL;
    f->ioctl           = NULL;
    f->ctx             = fd;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags           = 0;
    f->refcount        = 1;
    f->rights          = 0;
    f->ino             = ino;

    uint32_t pi = 0;
    if (path)
        while (pi < 255 && path[pi]) { f->path[pi] = path[pi]; pi++; }
    f->path[pi] = '\0';
    return f;
}

// ── ext2_open ──────────────────────────────────────────────────────────────

vfs_file_t* ext2_open(const char* path) {
    if (!s_mounted) return NULL;
    uint32_t ino = path_to_inode(path);
    if (!ino) return NULL;
    return ext2_open_by_ino(ino, path);
}

// ── ext2_open_ino ──────────────────────────────────────────────────────────
// Public: open by inode number when the caller already holds it (e.g. after
// ext2_lookup_path()).  Eliminates the second path walk in elf_exec_from_ext2
// and any other caller that already has the inode from a prior lookup.

vfs_file_t* ext2_open_ino(uint32_t ino, const char* path) {
    if (!s_mounted || !ino) return NULL;
    return ext2_open_by_ino(ino, path);
}

// ── ext2_readdir ───────────────────────────────────────────────────────────

// Bytes of an on-disk dirent's name that are safe to read: the smaller of the
// claimed name_len and the bytes left in the block after the 8-byte dirent
// header.  A corrupt/short on-disk dirent could otherwise claim a name_len that
// runs past the end of the 4096-byte bcache slot -> out-of-bounds read (leaking
// adjacent kernel memory into the returned filename).  Pure + bounds-only so it
// can be unit-tested in isolation (ext2_readdir_clamp_selftest).
uint32_t ext2_dirent_namelen_clamp(uint32_t off, uint32_t name_len,
                                   uint32_t blk_bytes) {
    uint32_t avail = (off + 8u <= blk_bytes) ? (blk_bytes - off - 8u) : 0u;
    return name_len < avail ? name_len : avail;
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic test of the dirent name-length bound (the security boundary
// that stops a corrupt on-disk dirent over-reading past the 4096-byte bcache
// slot).  The 4088/4090 tail cases are exactly the over-read scenarios: the
// header fits but the name would start at / past the slot end -> clamp to 0.
void ext2_readdir_clamp_selftest(void) {
    extern void kprintf(const char*, ...);
    kprintf("[ext2_readdir_test] dirent name_len clamp bounds\n");
    struct { uint32_t off, nlen, blk, want; } c[] = {
        { 0,    5,   4096, 5  },   // fits -> unchanged
        { 4088, 200, 4096, 0  },   // header ends at slot end, name OOB -> 0
        { 4090, 1,   4096, 0  },   // header itself overruns -> 0
        { 4078, 200, 4096, 10 },   // only 10 bytes left in block
        { 4000, 255, 4096, 88 },   // avail = 4096-4000-8
        { 100,  50,  120,  12 },   // short final block: avail = 120-100-8
        { 0,    0,   4096, 0  },   // empty name
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        uint32_t got = ext2_dirent_namelen_clamp(c[i].off, c[i].nlen, c[i].blk);
        if (got != c[i].want) {
            kprintf("[ext2_readdir_test] FAIL off=%u nlen=%u blk=%u got=%u want=%u\n",
                    c[i].off, c[i].nlen, c[i].blk, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_readdir_test] SELF-TEST FAILED\n"
                  : "[ext2_readdir_test] SELF-TEST PASSED (name clamped to block bounds)\n");
}

// Deterministic test of the mount-time block-size validation: only log 0/1/2
// (1024/2048/4096) are accepted, and every accepted size is <= the fixed
// buffer max.  log 3 (8192) is the first overflow; 31/32 are the shift-UB edge.
void ext2_block_size_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint32_t log, want; } c[] = {
        { 0,  1024 },   // 1024
        { 1,  2048 },   // 2048
        { 2,  4096 },   // 4096 == EXT2_BLOCK_SIZE_MAX
        { 3,  0    },   // 8192 -> rejected (would overrun the 4096 buffers)
        { 10, 0    },   // huge -> rejected
        { 31, 0    },   // rejected
        { 32, 0    },   // shift-UB region -> rejected
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        uint32_t got = ext2_block_size_checked(c[i].log);
        if (got != c[i].want || (got != 0 && got > EXT2_BLOCK_SIZE_MAX)) {
            kprintf("[ext2_blksz_test] FAIL log=%u got=%u want=%u\n",
                    c[i].log, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_blksz_test] SELF-TEST FAILED\n"
                  : "[ext2_blksz_test] SELF-TEST PASSED (block size validated <= 4096)\n");
}

// Pure test of ext2_group_geom_valid (the bitmap-OOB mount guard): blocks- and
// inodes-per-group must be in [1, block_size*8] so the in-group bit index stays
// within the one-block (<= 4096-byte) bitmap scratch.
void ext2_group_geom_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint32_t bpg, ipg, bs, want; } c[] = {
        { 8192,  8192,  1024, 1 },   // 1 KiB blocks: cap 8192 -> legit max passes
        { 8193,  100,   1024, 0 },   // blocks_per_group over cap -> reject
        { 100,   8193,  1024, 0 },   // inodes_per_group over cap -> reject
        { 32768, 32768, 4096, 1 },   // 4 KiB blocks: cap 32768 -> legit max passes
        { 32769, 100,   4096, 0 },   // over cap -> reject
        { 0,     100,   1024, 0 },   // zero blocks -> reject
        { 100,   0,     1024, 0 },   // zero inodes -> reject
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int got = ext2_group_geom_valid(c[i].bpg, c[i].ipg, c[i].bs);
        if ((got != 0) != (c[i].want != 0)) {
            kprintf("[ext2_geom] FAIL bpg=%u ipg=%u bs=%u got=%d want=%u\n",
                    c[i].bpg, c[i].ipg, c[i].bs, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_geom] SELF-TEST FAILED\n"
                  : "[ext2_geom] SELF-TEST PASSED (group geometry bounded to block_size*8)\n");
}
#endif

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
                // Clamp to what actually fits in this block so a corrupt
                // dirent's name_len can't over-read past the bcache slot.
                uint32_t nlen = ext2_dirent_namelen_clamp(off, de->name_len, blk_bytes);
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
            // Untrusted on-disk rec_len/name_len: a corrupt entry would underflow the
            // slack below and write the split entry past dae_buf (heap OOB).  A
            // malformed entry stops the walk (we append to a fresh block instead).
            if (!ext2_dirent_in_block(off, de->rec_len, de->name_len, blk_bytes)) break;

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
    inode_pub_begin(dir_leaf);
    dir_leaf->inode = dir_inode;
    inode_pub_end(dir_leaf);
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
            // Untrusted on-disk rec_len/name_len: bound the entry to the block so the
            // kmemeq name compare below cannot read past dre_buf (heap OOB read).
            if (!ext2_dirent_in_block(off, de->rec_len, de->name_len, blk_bytes)) break;

            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                kmemeq(de->name, name, name_len)) {
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

// Find the ".." entry in a directory block and repoint it at new_ino.  The
// untrusted on-disk rec_len/name_len are bounded to the block (the same
// ext2_dirent_in_block discipline dir_remove_entry uses) so the scan cannot read
// past `block`.  Only the inode field is touched -- rec_len/name_len/name are
// left intact so the entry stays valid.  Returns 1 if ".." was found+updated,
// 0 otherwise.  Pure (no I/O) -> unit-tested by ext2_dotdot_repoint_selftest.
static uint8_t dirent_repoint_dotdot(uint8_t* block, uint32_t blk_bytes,
                                     uint32_t new_ino) {
    uint32_t off = 0;
    while (off + 8 <= blk_bytes) {
        ext2_dirent_t* de = (ext2_dirent_t*)(block + off);
        if (de->rec_len == 0) break;
        if (!ext2_dirent_in_block(off, de->rec_len, de->name_len, blk_bytes)) break;
        if (de->inode != 0 && de->name_len == 2 &&
            de->name[0] == '.' && de->name[1] == '.') {
            de->inode = new_ino;
            return 1;
        }
        off += de->rec_len;
    }
    return 0;
}

// Repoint a directory's ".." entry at new_parent_ino.  Used by rename when a
// directory moves to a different parent: ".." lives in the directory's first
// data block.  Returns 1 on success.  Caller holds s_rename_lock; this takes the
// directory's own inode lock for the read-modify-write of its data block.
static uint8_t dir_set_dotdot(uint32_t dir_ino_num, uint32_t new_parent_ino) {
    irtree_leaf_t* dir_leaf = inode_lock(dir_ino_num);
    if (!dir_leaf) return 0;
    ext2_inode_t dir_inode = dir_leaf->inode;
    uint8_t ok = 0;
    uint32_t blk = inode_get_block(&dir_inode, 0);   // ".." is in block 0
    if (blk) {
        uint8_t* buf = (uint8_t*)kmalloc(4096);
        if (buf) {
            if (read_block(blk, buf)) {
                uint32_t blk_bytes = (dir_inode.i_size < s_block_size)
                                     ? dir_inode.i_size : s_block_size;
                if (dirent_repoint_dotdot(buf, blk_bytes, new_parent_ino)) {
                    write_block(blk, buf);
                    ok = 1;
                }
            }
            kfree(buf);
        }
    }
    inode_unlock(dir_leaf);
    return ok;
}

// ── Path utilities ─────────────────────────────────────────────────────────

// Split path into parent path and basename.
// parent_out must be >= str_len(path)+1 bytes.
// Returns the basename pointer within `path`.
// Max path length the FS handles (matches sys_open's char[512] user-path copy).
// Every path_split parent buffer is sized to this so a path sys_open accepts is
// never spuriously rejected, while the explicit `cap` argument keeps the copy
// in bounds REGARDLESS of buffer size (defense in depth).
#define EXT2_PATH_MAX 512

// Split `path` into its parent directory (written to parent_out, a `cap`-byte
// buffer) and basename (the returned suffix).  Returns NULL if the parent does
// not fit in `cap` bytes -- previously the copy had NO bound, so a path longer
// than the caller's fixed buffer (reachable via open(O_CREAT) with a ~511-byte
// path) overran the kernel stack and smashed the saved return address.
static const char* path_split(const char* path, char* parent_out, uint32_t cap) {
    if (cap < 2) return NULL;            // need room for at least "/" + NUL
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

    // Bound the copy: we write parent_out[0..last_slash-1] + a NUL at
    // [last_slash], so we need last_slash < cap.  Reject (NULL) otherwise;
    // every caller already treats a NULL return as an error.
    if ((uint32_t)last_slash >= cap) return NULL;
    for (int i = 0; i < last_slash; i++) parent_out[i] = path[i];
    parent_out[last_slash] = '\0';
    return path + last_slash + 1;
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic test of the physical block-number range checks that stop an
// untrusted on-disk block pointer from becoming a wild device LBA.  Drives the
// pure helpers with a fixed filesystem shape (first-data-block 1, 100 blocks)
// so no mounted image is needed; covers the boundary + the overflow-safe run.
void ext2_block_valid_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    // Single block in [1,100): 0 and >=100 (and 0xFFFFFFFF) are out of range.
    struct { uint32_t blk; int want; } b[] = {
        { 0, 0 }, { 1, 1 }, { 50, 1 }, { 99, 1 }, { 100, 0 }, { 0xFFFFFFFFu, 0 },
    };
    for (unsigned i = 0; i < sizeof(b)/sizeof(b[0]); i++) {
        int got = ext2_block_in_range(b[i].blk, 1u, 100u);
        if (got != b[i].want) {
            kprintf("[ext2_blkvalid] FAIL blk=%u got=%d want=%d\n",
                    b[i].blk, got, b[i].want);
            fails++;
        }
    }
    // Run [start, start+run) within [1,100), including a uint32 wrap case.
    struct { uint32_t start, run; int want; } r[] = {
        { 1, 99, 1 },                 // 1..99 fits exactly
        { 1, 100, 0 },                // would reach block 100 -> reject
        { 50, 50, 1 },                // 50..99 fits
        { 50, 51, 0 },                // reaches 100 -> reject
        { 99, 1, 1 },                 // just block 99
        { 99, 2, 0 },                 // reaches 100 -> reject
        { 1, 0, 1 },                  // empty run is vacuously ok
        { 0xFFFFFF00u, 0x200u, 0 },   // start+run wraps uint32 -> reject
    };
    for (unsigned i = 0; i < sizeof(r)/sizeof(r[0]); i++) {
        int got = ext2_run_in_range(r[i].start, r[i].run, 1u, 100u);
        if (got != r[i].want) {
            kprintf("[ext2_blkvalid] FAIL run start=%u run=%u got=%d want=%d\n",
                    r[i].start, r[i].run, got, r[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_blkvalid] SELF-TEST FAILED\n"
                  : "[ext2_blkvalid] SELF-TEST PASSED (block + run range, overflow-safe)\n");
}

// ext2_blk_lba forms the device LBA in 64-bit so blk*spb cannot wrap a u32 and
// hit a wrong sector.  Drive the WRAP boundary explicitly: a 32-bit product
// would truncate these, a correct 64-bit one does not.
void ext2_blk_lba_selftest(void) {
    kprintf("[ext2_blklba] ext2_blk_lba must form a 64-bit LBA without u32 wrap\n");
    int fails = 0;
    struct { uint32_t part, blk, spb; uint64_t want; } c[] = {
        { 2,       0,           8, 2ULL },            // blk 0 -> just the partition base
        { 2,       100,         8, 802ULL },          // normal small block
        { 0,       0x20000000u, 8, 0x100000000ULL },  // blk*spb == 2^32: u32 would wrap to 0
        { 0x1000,  0x20000000u, 8, 0x100001000ULL },  // + part base, still past 2^32
        { 0,       0xFFFFFFFFu, 8, 0x7FFFFFFF8ULL },   // max block number, no wrap
        { 7,       0x10000000u, 4, 0x40000007ULL },    // mixed spb=4
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        uint64_t got = ext2_blk_lba(c[i].part, c[i].blk, c[i].spb);
        if (got != c[i].want) {
            kprintf("[ext2_blklba] FAIL part=0x%lx blk=0x%lx spb=%u got=0x%lx want=0x%lx\n",
                    (unsigned long)c[i].part, (unsigned long)c[i].blk, c[i].spb,
                    (unsigned long)got, (unsigned long)c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_blklba] SELF-TEST FAILED\n"
                  : "[ext2_blklba] SELF-TEST PASSED (64-bit LBA, no u32 wrap)\n");
}

// ext2_inode_size_valid gates the untrusted s_inode_size at mount so an inode can
// never straddle the bcache slot (the OOB read/write at inode_load_into /
// inode_disk_write).  Drive the reject cases a `> 0` check would have let through.
void ext2_inode_size_valid_selftest(void) {
    kprintf("[ext2_isz] ext2_inode_size_valid must reject non-pow2 / too-small / too-big\n");
    int fails = 0;
    struct { uint32_t isz, bsz; int want; } c[] = {
        { 128,  4096, 1 },   // standard inode
        { 256,  4096, 1 },   // large inode, power of 2
        { 4096, 4096, 1 },   // inode == block (1 per block)
        { 128,  1024, 1 },   // smallest supported block
        { 0,    4096, 0 },   // zero
        { 64,   4096, 0 },   // power of 2 but < sizeof(ext2_inode_t)
        { 192,  4096, 0 },   // in range but NOT a power of two
        { 4032, 4096, 0 },   // the crafted-straddle case (not a power of two)
        { 8192, 4096, 0 },   // larger than the block
        { 130,  4096, 0 },   // not a power of two
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int got = ext2_inode_size_valid(c[i].isz, c[i].bsz);
        if (got != c[i].want) {
            kprintf("[ext2_isz] FAIL isz=%u bsz=%u got=%d want=%d\n",
                    c[i].isz, c[i].bsz, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_isz] SELF-TEST FAILED\n"
                  : "[ext2_isz] SELF-TEST PASSED (inode-size pow2 + range validation)\n");
}

// ext2_dirent_in_block gates the untrusted on-disk rec_len/name_len so dir_add_entry's
// slack split (and dir_remove_entry's name compare) stay inside the 4096-byte block.
// Drive the underflow + past-block reject cases a bare walk would have written/read OOB.
void ext2_dirent_in_block_selftest(void) {
    kprintf("[ext2_dirent] ext2_dirent_in_block must reject corrupt rec_len/name_len\n");
    int fails = 0;
    struct { uint32_t off, rec, nlen, blk, want; } c[] = {
        { 0,    16,     5,   4096, 1 },   // valid: actual align(13)=16 == rec, in-block
        { 0,    12,     4,   4096, 1 },   // valid: actual 12 == rec
        { 4080, 16,     5,   4096, 1 },   // valid: exact fit, off+rec == blk
        { 2048, 2048,   200, 4096, 1 },   // valid: large entry filling half the block
        { 0,    16,     255, 4096, 0 },   // actual align(263)=264 > rec 16 -> slack underflow
        { 4088, 16,     0,   4096, 0 },   // off+rec 4104 > blk -> runs past the block
        { 0,    4,      0,   4096, 0 },   // rec_len 4 < 8 header -> reject
        { 0,    0xFFFF, 10,  4096, 0 },   // huge rec_len past the block
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int got = ext2_dirent_in_block(c[i].off, c[i].rec, c[i].nlen, c[i].blk);
        if (got != (int)c[i].want) {
            kprintf("[ext2_dirent] FAIL off=%u rec=%u nlen=%u got=%d want=%u\n",
                    c[i].off, c[i].rec, c[i].nlen, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[ext2_dirent] SELF-TEST FAILED\n"
                  : "[ext2_dirent] SELF-TEST PASSED (dirent rec_len/name_len bounds)\n");
}

// Deterministic test of dirent_repoint_dotdot (the F68 cross-parent dir-rename
// fix core): in a constructed dir block it must repoint ONLY the ".." entry's
// inode, leaving "."/sibling entries and all rec_len/name_len metadata intact,
// and report not-found when there is no "..".
void ext2_dotdot_repoint_selftest(void) {
    extern void kprintf_atomic(const char*, ...);
    int fails = 0;
    const uint32_t bb = 1024;            // block_size-independent: pass blk_bytes
    // Heap-allocate the block buffer: the kernel stack is only 8KiB (KSTACK_PAGES
    // = 2), so a 4096-byte stack array would gut it -- every dir-block buffer in
    // this file is kmalloc'd for exactly this reason.
    uint8_t* blk = (uint8_t*)kmalloc(4096);
    if (!blk) { kprintf_atomic("[ext2_dotdot] SELF-TEST FAILED (alloc)\n"); return; }

    // Block: "." (ino 10), ".." (ino 20), "file" (ino 30, fills the rest).
    __builtin_memset(blk, 0, 4096);
    ext2_dirent_t* d  = (ext2_dirent_t*)(blk + 0);
    d->inode = 10; d->rec_len = 12; d->name_len = 1; d->file_type = EXT2_FT_DIR;
    d->name[0] = '.';
    ext2_dirent_t* dd = (ext2_dirent_t*)(blk + 12);
    dd->inode = 20; dd->rec_len = 12; dd->name_len = 2; dd->file_type = EXT2_FT_DIR;
    dd->name[0] = '.'; dd->name[1] = '.';
    ext2_dirent_t* f  = (ext2_dirent_t*)(blk + 24);
    f->inode = 30; f->rec_len = (uint16_t)(bb - 24); f->name_len = 4;
    f->file_type = EXT2_FT_REG_FILE;
    f->name[0]='f'; f->name[1]='i'; f->name[2]='l'; f->name[3]='e';

    if (!dirent_repoint_dotdot(blk, bb, 99)) {
        fails++; kprintf_atomic("[ext2_dotdot] FAIL .. not found\n"); }
    if (dd->inode != 99) {
        fails++; kprintf_atomic("[ext2_dotdot] FAIL .. inode=%u (want 99)\n", dd->inode); }
    if (dd->name_len != 2 || dd->rec_len != 12 || dd->name[0] != '.' || dd->name[1] != '.') {
        fails++; kprintf_atomic("[ext2_dotdot] FAIL .. metadata clobbered\n"); }
    if (d->inode != 10) { fails++; kprintf_atomic("[ext2_dotdot] FAIL . clobbered\n"); }
    if (f->inode != 30) { fails++; kprintf_atomic("[ext2_dotdot] FAIL sibling clobbered\n"); }

    // Block with NO ".." (only "." and a sibling) -> not found, nothing changes.
    __builtin_memset(blk, 0, 4096);
    d = (ext2_dirent_t*)(blk + 0);
    d->inode = 10; d->rec_len = 12; d->name_len = 1; d->name[0] = '.';
    f = (ext2_dirent_t*)(blk + 12);
    f->inode = 30; f->rec_len = (uint16_t)(bb - 12); f->name_len = 1; f->name[0] = 'x';
    if (dirent_repoint_dotdot(blk, bb, 99) != 0) {
        fails++; kprintf_atomic("[ext2_dotdot] FAIL spurious match (no ..)\n"); }
    if (f->inode != 30 || d->inode != 10) {
        fails++; kprintf_atomic("[ext2_dotdot] FAIL touched entries with no ..\n"); }

    kfree(blk);
    kprintf_atomic(fails ? "[ext2_dotdot] SELF-TEST FAILED\n"
                         : "[ext2_dotdot] PASS (.. repointed, siblings + metadata intact)\n");
}

// Deterministic test of path_split's bound: a parent longer than the
// destination buffer must be REJECTED (return NULL), never copied (the kernel
// stack overflow this fixes).  Also checks normal splits + the exact boundary.
void ext2_path_split_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    char buf[EXT2_PATH_MAX];

    // Tiny inline compare (no str_eq in this TU).
    #define EQ(a,b) ({ const char* _x=(a); const char* _y=(b); \
                       while (*_x && *_x==*_y) { _x++; _y++; } *_x==*_y; })

    const char* b;
    b = path_split("/foo/bar", buf, sizeof(buf));        // normal split
    if (!b || !EQ(b,"bar") || !EQ(buf,"/foo")) {
        kprintf("[ext2_pathsplit] FAIL normal b=%p\n", (void*)b); fails++; }

    b = path_split("/a", buf, sizeof(buf));              // root parent
    if (!b || !EQ(b,"a") || !EQ(buf,"/")) {
        kprintf("[ext2_pathsplit] FAIL root b=%p\n", (void*)b); fails++; }

    // "/abcdefg/x": last '/' at index 8, parent "/abcdefg" is 8 chars -> needs
    // 9 bytes (8 + NUL).  cap 9 fits exactly; cap 8 must reject.
    b = path_split("/abcdefg/x", buf, 9u);
    if (!b || !EQ(buf,"/abcdefg")) {
        kprintf("[ext2_pathsplit] FAIL fit-9 b=%p\n", (void*)b); fails++; }
    b = path_split("/abcdefg/x", buf, 8u);
    if (b != (const char*)0) {                            // MUST reject, not overflow
        kprintf("[ext2_pathsplit] FAIL overflow-8 not rejected\n"); fails++; }

    // The bug case: a parent far larger than cap must reject with no copy.
    b = path_split("/aaaaaaaaaaaaaaaaaaaa/x", buf, 8u);   // parent 20+ chars, cap 8
    if (b != (const char*)0) {
        kprintf("[ext2_pathsplit] FAIL long-parent not rejected\n"); fails++; }

    #undef EQ
    kprintf(fails ? "[ext2_pathsplit] SELF-TEST FAILED\n"
                  : "[ext2_pathsplit] SELF-TEST PASSED (parent bound, no stack overflow)\n");
}
#endif /* MAKAOS_BOOT_SELFTESTS */

// ── ext2_write_file ────────────────────────────────────────────────────────

// True (0) iff `cred` may add/remove entries in directory `dir_ino` (write+exec).
// Defined after the errno.h include below; forward-declared here so the mutating
// ops can gate on the SAME parent inode they resolve (closes the path-TOCTOU).
static int ext2_dir_write_ok(uint32_t dir_ino, const void* cred);

int ext2_write_file(const char* path, const uint8_t* data, uint32_t size,
                    const void* cred) {
    if (!s_mounted) return 0;

    // Split into parent dir and basename.
    char parent_path[EXT2_PATH_MAX];
    const char* basename = path_split(path, parent_path, sizeof(parent_path));
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;

    // Check if file already exists.
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;

    uint32_t existing_ino = dir_lookup(&parent_inode, basename, str_len(basename));

    // Creating a NEW file (existing_ino == 0) requires write+exec on THIS
    // parent -- checked on the same parent_ino this call resolved and will
    // dir_add_entry into, so a concurrent rename of an intermediate path
    // component cannot land the create in a directory the caller was never
    // authorized for (the path-TOCTOU).  A NULL cred (internal overwrite /
    // truncate of an EXISTING file) skips it; it gates new-file creation only.
    if (!existing_ino && cred && ext2_dir_write_ok(parent_ino, cred) != 0) return 0;

    // Scratch buffer reused for every block write below.  Lazily alloc'd
    // only after we've passed all the early-error checks in each branch.
    EXT2_SCRATCH_DECL(wr_buf);

    if (existing_ino) {
        // Overwrite.  Hold the per-inode lock across the ENTIRE free -> realloc
        // -> writeback.  Previously this ran on a read_inode SNAPSHOT with the
        // lock taken only for the final writeback, so two concurrent writers to
        // the same file (two O_TRUNC opens, or ftruncate+write) both snapshotted
        // the same i_block[] and both ran free_inode_blocks -- the second freed
        // blocks the first had already freed AND re-allocated, handing a live
        // block back to the bitmap to be re-allocated to ANOTHER file (cross-file
        // corruption).  inode_lock is the per-inode write_lock (serialises
        // writers) + seqlock; the bitmap/bcache ops below never take inode_lock
        // (the same lock order the final writeback already used), so no cycle.
        uint32_t n_blocks = (size + s_block_size - 1) / s_block_size;

        // Alloc the scratch BEFORE the lock: EXT2_SCRATCH_ALLOC has an embedded
        // `return 0` on OOM, which under the lock would leak it.
        if (n_blocks > 0) EXT2_SCRATCH_ALLOC(wr_buf);

        irtree_leaf_t* leaf = inode_lock(existing_ino);
        if (!leaf) return 0;

        // Work on a LOCAL copy of the (now lock-stable) inode so an error mid-way
        // discards cleanly, exactly as the read_inode snapshot did before; the
        // single inode_unlock + `return ok` below is the only exit under the lock.
        ext2_inode_t work = leaf->inode;
        free_inode_blocks(&work);

        uint32_t written = 0;
        int ok = 1;
        for (uint32_t bi = 0; bi < n_blocks; bi++) {
            uint32_t blk = alloc_block();
            if (!blk) { ok = 0; break; }

            uint32_t to_write = size - written;
            if (to_write > s_block_size) to_write = s_block_size;

            __builtin_memcpy(wr_buf, data + written, to_write);
            __builtin_memset(wr_buf + to_write, 0, s_block_size - to_write);

            if (!write_block(blk, wr_buf))        { free_block(blk); ok = 0; break; }
            if (!inode_set_block(&work, bi, blk)) { free_block(blk); ok = 0; break; }

            written += to_write;
        }

        if (ok) {
            work.i_size        = size;
            work.i_blocks      = n_blocks * (s_block_size / 512);
            work.i_mode        = EXT2_S_IFREG | 0644;
            work.i_links_count = 1;
            inode_pub_begin(leaf);
            leaf->inode = work;          // commit to the cache only on success
            inode_pub_end(leaf);
            inode_writeback(leaf);
        }
        inode_unlock(leaf);
        return ok;
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
            inode_pub_begin(leaf);
            leaf->inode = new_inode;
            leaf->valid = 1;
            inode_pub_end(leaf);
            inode_writeback(leaf);
            inode_unlock(leaf);
        }
    }

    // Add directory entry in parent.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_REG_FILE)) {
        free_inode_num(new_ino);
        return 0;
    }

    // Phase 7C: create-via-write path — invalidate any negative
    // dentry that cached ENOENT for this file.
    dcache_invalidate(parent_ino, basename, str_len(basename));

    return 1;
}

// ── ext2_create ───────────────────────────────────────────────────────────
// Create an empty file.  Returns 0 if the file already exists.
int ext2_create(const char* path, const void* cred) {
    if (!s_mounted) return 0;
    char parent_path[EXT2_PATH_MAX];
    const char* basename = path_split(path, parent_path, sizeof(parent_path));
    if (!basename || basename[0] == '\0') return 0;
    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;
    // Fail if already exists.
    if (dir_lookup(&parent_inode, basename, str_len(basename))) return 0;
    // Create empty file via ext2_write_file; pass cred so the parent write+exec
    // check happens on ext2_write_file's OWN parent resolution (the create site).
    int r = ext2_write_file(path, (const uint8_t*)"", 0, cred);
    // Phase 7C: invalidate any negative dentry that cached ENOENT
    // at this path.  After create, the path exists — future lookups
    // should re-resolve and install a positive dentry.
    if (r) dcache_invalidate(parent_ino, basename, str_len(basename));
    return r;
}

// ── ext2_truncate ─────────────────────────────────────────────────────────
// Truncate file to zero bytes.
int ext2_truncate(const char* path) {
    return ext2_write_file(path, (const uint8_t*)"", 0, NULL);  // existing file: no create-perm check
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
        int r = ext2_write_file(path, buf, new_size, NULL);  // existing file: no create-perm check
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
        int r = ext2_write_file(path, buf, new_size, NULL);  // existing file: no create-perm check
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

// ── ext2_dir_write_ok ───────────────────────────────────────────────────────
// Returns 0 iff `cred` (a cred_t*) may create/remove directory entries in
// directory inode `dir_ino` -- i.e. write+exec on the dir -- else a negative
// errno.  The mutating ops (unlink/mkdir/rename/create-via-write_file) call this
// on the SAME parent inode they themselves resolved and are about to modify, so
// the permission decision and the modification are on ONE resolution.  That
// closes the path-TOCTOU: the syscall-layer check resolves the parent
// separately, and a concurrent rename of an intermediate path component could
// otherwise make the op act on a different parent than was permission-checked.
static int ext2_dir_write_ok(uint32_t dir_ino, const void* cred) {
    ext2_inode_t din;
    if (!read_inode(dir_ino, &din)) return -ENOENT;
    inode_perm_t ip = {
        .uid = din.i_uid, .gid = din.i_gid,
        .mode = din.i_mode & 0x1FF,
        .inode_nr = dir_ino, .dev = 0, .nosuid = 0,
    };
    return vfs_check_perm(&ip, (const cred_t*)cred, ACL_PERM_WRITE | ACL_PERM_EXEC);
}

// Phase 7B: dcache-accelerated path walk.
//
// For each path component:
//   1. Compute name hash.
//   2. rcu-protected dcache lookup for (cur_ino, name, hash).
//   3. On hit: use cached child_ino (or fail for DCACHE_NEGATIVE).
//      Still read the parent inode to check the EXEC bit against the
//      caller's creds — cached perms per-cred would be another layer
//      and inode read is already cheap (seqlock reader on irtree).
//   4. On miss: do the original O(N) dir_lookup + install a positive
//      or negative dentry for next time.
//
// Net effect: repeated walks of the same path skip the per-component
// directory-block scan entirely (dir_lookup is the single biggest
// cost in path walk).
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

        const char* name      = path + start;
        uint32_t    name_hash = dcache_name_hash(name, comp_len);

        // Read parent inode for dir-type + EXEC-perm check.  Fast:
        // irtree hit is a seqlock read (zero atomics).
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

        // Try the dcache.
        dentry_t* d = dcache_lookup(cur_ino, name, comp_len, name_hash);
        if (d) {
            uint32_t child = d->child_ino;
            dcache_put(d);
            if (child == DCACHE_NEGATIVE) {
                if (err_out) *err_out = -ENOENT;
                return 0;
            }
            cur_ino = child;
        } else {
            // Miss — fall through to on-disk directory scan.
            uint32_t next_ino = dir_lookup(&cur_inode, name, comp_len);
            if (!next_ino) {
                // Install negative dentry so future lookups of the
                // same path skip the scan.
                dentry_t* neg = dcache_install(NULL, cur_ino, name, comp_len,
                                                name_hash, DCACHE_NEGATIVE);
                if (neg) dcache_put(neg);
                if (err_out) *err_out = -ENOENT;
                return 0;
            }
            // Install positive dentry.
            dentry_t* pos = dcache_install(NULL, cur_ino, name, comp_len,
                                            name_hash, next_ino);
            if (pos) dcache_put(pos);
            cur_ino = next_ino;
        }

        if (path[i] == '/') i++;
    }

    return cur_ino;
}

uint8_t ext2_read_inode(uint32_t ino, ext2_inode_t* out) {
    return read_inode(ino, out);
}

// ── ext2_mkdir ────────────────────────────────────────────────────────────

int ext2_mkdir(const char* path, const void* cred) {
    if (!s_mounted) return 0;

    // Check it doesn't already exist.
    if (path_to_inode(path)) return 0; // already exists

    char md_parent[EXT2_PATH_MAX];
    const char* basename = path_split(path, md_parent, sizeof(md_parent));
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(md_parent);
    if (!parent_ino) return 0;

    // Write+exec on the resolved parent, BEFORE allocating anything (so a denial
    // leaks nothing) and on the same parent_ino we dir_add_entry into below --
    // closes the path-TOCTOU vs the syscall-layer check's separate resolution.
    if (cred && ext2_dir_write_ok(parent_ino, cred) != 0) return 0;

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
            inode_pub_begin(leaf);
            leaf->inode = new_inode;
            leaf->valid = 1;
            inode_pub_end(leaf);
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
            inode_pub_begin(pleaf);
            pleaf->inode.i_links_count++;
            inode_pub_end(pleaf);
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

    // Phase 7C: any negative dentry at (parent, basename) is now stale
    // — the directory exists.  Invalidating lets the next lookup
    // install a fresh positive dentry pointing at new_ino.
    dcache_invalidate(parent_ino, basename, str_len(basename));

    return 1;
}

// ── link-count drop (shared by unlink + rename dst-removal) ─────────────────
// Pure: drop one hard link.  Returns 1 if the inode should now be freed (the
// count reached 0), 0 if other names still reference it.  The `> 0` guard
// stops a corrupt/0 count from wrapping the u16 to 65535; a drop at 0 reports
// "free" so an already-orphaned inode is reclaimed rather than leaked.
static int ext2_link_drop(uint16_t* links) {
    if (*links > 0) (*links)--;
    return (*links == 0);
}

// Drop one link from an already-inode_lock'd leaf and free its inode + data
// blocks ONLY when the link count reaches 0 (otherwise just write back the
// decremented count).  Always consumes the lock.  This is the single
// count-aware free site shared by ext2_unlink and ext2_rename's dst-removal so
// the two cannot drift -- the rename path used to free unconditionally, which
// destroyed a still-referenced multi-link inode.
static void ext2_drop_link_locked(irtree_leaf_t* leaf, uint32_t ino) {
    // Work on a LOCAL copy: free_inode_blocks does bitmap I/O while clearing
    // i_block[]/i_blocks, so mutating the cached leaf->inode in place would let
    // a reader see a half-freed inode (the sequence is even during I/O now).
    // Compute the final inode off-cache, publish it atomically (brief seq
    // bracket, no I/O), then persist.
    ext2_inode_t work = leaf->inode;
    uint16_t links = work.i_links_count;   // local: avoid &packed-member
    int freed = ext2_link_drop(&links);
    work.i_links_count = links;
    if (freed) {
        free_inode_blocks(&work);
        work.i_dtime = 1;
    }
    inode_pub_begin(leaf);
    leaf->inode = work;
    inode_pub_end(leaf);
    inode_writeback(leaf);
    inode_unlock(leaf);
    if (freed) free_inode_num(ino);
}

// Deterministic check of the link-drop decision shared by unlink + rename:
// only the drop that takes the count to 0 reports "free", and a corrupt/0
// count never wraps.  This is the core of the fix -- rename used to skip this
// logic entirely and free a multi-link inode on every dst-removal.
void ext2_link_drop_selftest(void) {
    extern void kprintf_atomic(const char*, ...);
    int fails = 0;
    uint16_t c;
    c = 2; if (ext2_link_drop(&c) != 0 || c != 1) fails++;   // 2->1: keep
    c = 1; if (ext2_link_drop(&c) != 1 || c != 0) fails++;   // 1->0: free
    c = 3;                                                    // 3->2->1->0
    if (ext2_link_drop(&c) != 0 || c != 2) fails++;
    if (ext2_link_drop(&c) != 0 || c != 1) fails++;
    if (ext2_link_drop(&c) != 1 || c != 0) fails++;
    c = 0; if (ext2_link_drop(&c) != 1 || c != 0) fails++;   // underflow guard: stays 0
    if (fails)
        kprintf_atomic("[ext2_link_drop] SELF-TEST FAILED (%d)\n", fails);
    else
        kprintf_atomic("[ext2_link_drop] PASS (free only at links==0, no u16 underflow)\n");
}

// ── ext2_unlink ───────────────────────────────────────────────────────────
// Remove a regular file at `path`.  Returns 1 on success, 0 on failure.
int ext2_unlink(const char* path, const void* cred) {
    if (!s_mounted || !path) return 0;

    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;

    char ul_parent[EXT2_PATH_MAX];
    const char* basename = path_split(path, ul_parent, sizeof(ul_parent));
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(ul_parent);
    if (!parent_ino) return 0;

    // Write+exec on the resolved parent, on the same parent_ino we
    // dir_remove_entry from below -- closes the path-TOCTOU vs the syscall-layer
    // check's separate resolution (a swapped intermediate dir is now denied).
    if (cred && ext2_dir_write_ok(parent_ino, cred) != 0) return 0;

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
    ext2_drop_link_locked(leaf, ino);

    // Phase 7C: invalidate the dentry that cached this path component.
    // Future lookups for (parent_ino, basename) will miss dcache and
    // either find a new entry (if another inode got reinstalled under
    // the same name) or install a negative dentry.
    dcache_invalidate(parent_ino, basename, str_len(basename));

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
int ext2_rename(const char* src, const char* dst, const void* cred) {
    if (!s_mounted || !src || !dst) return 0;

    spin_lock(&s_rename_lock);

    uint32_t src_ino = path_to_inode(src);
    if (!src_ino) { spin_unlock(&s_rename_lock); return 0; }

    ext2_inode_t src_inode;
    if (!read_inode(src_ino, &src_inode)) { spin_unlock(&s_rename_lock); return 0; }

    uint8_t is_dir = ((src_inode.i_mode & 0xF000) == EXT2_S_IFDIR);

    // Resolve src parent/basename.
    char rn_src_parent[EXT2_PATH_MAX];
    const char* src_base = path_split(src, rn_src_parent, sizeof(rn_src_parent));
    if (!src_base || src_base[0] == '\0') { spin_unlock(&s_rename_lock); return 0; }
    uint32_t src_parent_ino = path_to_inode(rn_src_parent);
    if (!src_parent_ino) { spin_unlock(&s_rename_lock); return 0; }

    // Resolve dst parent/basename.
    char rn_dst_parent[EXT2_PATH_MAX];
    const char* dst_base = path_split(dst, rn_dst_parent, sizeof(rn_dst_parent));
    if (!dst_base || dst_base[0] == '\0') { spin_unlock(&s_rename_lock); return 0; }
    uint32_t dst_parent_ino = path_to_inode(rn_dst_parent);
    if (!dst_parent_ino) { spin_unlock(&s_rename_lock); return 0; }

    // Write+exec on BOTH resolved parents -- removing the src entry needs write
    // on the src parent, adding the dst entry needs write on the dst parent.
    // Checked here on the same inodes the op modifies (closes the path-TOCTOU),
    // AND closes the gap where sys_rename only checked the SRC parent so a
    // rename INTO a no-write directory wrongly succeeded.
    if (cred && (ext2_dir_write_ok(src_parent_ino, cred) != 0 ||
                 ext2_dir_write_ok(dst_parent_ino, cred) != 0)) {
        spin_unlock(&s_rename_lock);
        return 0;
    }

    // If dst exists as a regular file, unlink it under its inode lock.
    uint32_t dst_ino = path_to_inode(dst);
    // rename(2): if src and dst resolve to the SAME file (same inode, including
    // two hard links to it), do nothing and succeed.  Without this guard the
    // dst-removal below frees the inode and its blocks, then dir_add_entry
    // re-adds a dirent pointing at the now-freed inode -- data loss + a dangling
    // directory entry.  (The dir-into-own-subtree case is rejected earlier in
    // sys_rename; here dst_ino is 0 for that case, so this does not mask it.)
    if (dst_ino && dst_ino == src_ino) { spin_unlock(&s_rename_lock); return 1; }
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
        // Remove dst's name and drop ONE link, freeing the inode + blocks only
        // when its link count reaches 0 (mirrors ext2_unlink via the shared
        // helper).  The old code freed unconditionally (i_links_count = 0 +
        // free_inode_blocks + free_inode_num), which destroyed a still-
        // referenced multi-link dst -- the surviving name then pointed at a
        // freed/reissued inode (cross-link).  Gate the drop on dir_remove_entry
        // succeeding: if a racing unlink(dst) already removed the name (and may
        // have freed the inode), it owns the drop and freeing here too would
        // double-free the inode bit + blocks.
        if (dir_remove_entry(dst_parent_ino, dst_base)) {
            dleaf = inode_lock(dst_ino);
            if (dleaf) ext2_drop_link_locked(dleaf, dst_ino);
        }
    }

    // Add new directory entry pointing at the same inode.
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (!dir_add_entry(dst_parent_ino, dst_base, src_ino, ft)) {
        spin_unlock(&s_rename_lock);
        return 0;
    }

    // Remove old directory entry.
    dir_remove_entry(src_parent_ino, src_base);

    // Moving a DIRECTORY to a different parent: its ".." backlink moves from the
    // old parent to the new one.  Repoint ".." (else it escapes to the old
    // parent -- /b/dir/.. would resolve to /a) and fix both parents' link
    // counts (each subdirectory contributes one ".." link to its parent).
    // Without this the old parent's count stays too high (it can never be
    // rmdir'd) and the new parent's too low (an over-decrement elsewhere could
    // free it while this dir's ".." still references it -- a cross-link).  A
    // same-parent rename touches neither.  RMW each parent under its own inode
    // lock, taken one at a time (no nesting) to match the existing lock order.
    if (is_dir && src_parent_ino != dst_parent_ino) {
        dir_set_dotdot(src_ino, dst_parent_ino);

        irtree_leaf_t* spleaf = inode_lock(src_parent_ino);
        if (spleaf) {
            inode_pub_begin(spleaf);
            if (spleaf->inode.i_links_count > 0) spleaf->inode.i_links_count--;
            inode_pub_end(spleaf);
            inode_writeback(spleaf);
            inode_unlock(spleaf);
        }
        irtree_leaf_t* dpleaf = inode_lock(dst_parent_ino);
        if (dpleaf) {
            inode_pub_begin(dpleaf);
            dpleaf->inode.i_links_count++;
            inode_pub_end(dpleaf);
            inode_writeback(dpleaf);
            inode_unlock(dpleaf);
        }
    }

    // Phase 7C: invalidate both the src dentry (moved away) and any
    // cached negative dentry at the dst (which may previously have
    // cached ENOENT).  If dst_ino was set we also killed an old
    // positive dentry for that path.
    dcache_invalidate(src_parent_ino, src_base, str_len(src_base));
    dcache_invalidate(dst_parent_ino, dst_base, str_len(dst_base));

    spin_unlock(&s_rename_lock);
    return 1;
}

// Deterministic guard for the SCAN #11 path-TOCTOU fix (F129): exercise the
// mutating ops with a ROOT cred and assert the in-op write+exec permission
// check (now on the op's OWN resolved parent) does NOT block allowed access and
// that create/unlink/mkdir/rename perform their state transitions.  Proves the
// fix did not break normal (authorized) file mutation -- the part a clean boot
// alone does not strongly exercise.  Self-cleaning (the one leftover scratch dir
// is harmless; the disk image is rebuilt from the source tree each boot).
void ext2_perm_op_selftest(void) {
    extern void kprintf(const char*, ...);
    if (!s_mounted) { kprintf("[fs_permop] SKIP (ext2 not mounted)\n"); return; }
    int fails = 0;
    cred_t root;
    __builtin_memset(&root, 0, sizeof root);   // euid/egid 0 -> root, passes write+exec

    // create -> resolves -> unlink -> gone.
    if (!ext2_create("/__fsperm_a", &root))      fails++;
    if (!path_to_inode("/__fsperm_a"))           fails++;
    if (!ext2_unlink("/__fsperm_a", &root))      fails++;
    if (path_to_inode("/__fsperm_a"))            fails++;

    // mkdir -> resolves (no rmdir primitive; the dir is harmless this boot).
    if (!ext2_mkdir("/__fsperm_d", &root))       fails++;
    if (!path_to_inode("/__fsperm_d"))           fails++;

    // create -> rename a->b -> old gone, new present -> unlink b.
    if (!ext2_create("/__fsperm_s", &root))                fails++;
    if (!ext2_rename("/__fsperm_s", "/__fsperm_t", &root)) fails++;
    if (path_to_inode("/__fsperm_s"))                      fails++;
    if (!path_to_inode("/__fsperm_t"))                     fails++;
    if (!ext2_unlink("/__fsperm_t", &root))               fails++;

    kprintf(fails ? "[fs_permop] SELF-TEST FAILED\n"
                  : "[fs_permop] SELF-TEST PASSED (root create/unlink/mkdir/rename via in-op perm check)\n");
}
