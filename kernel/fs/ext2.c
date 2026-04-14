#include "ext2.h"
#include "ahci.h"
#include "kheap.h"
#include "common.h"
#include "smp.h"

// ── Multi-task-safe scratch buffers ──────────────────────────────────────
// Function-local 4 KiB scratch buffers must NOT live on the kstack (8 KiB
// total) or in static/BSS (would race across preempted tasks).  Use the
// pattern below: declare the slot once at the top of the function, then
// alloc *lazily* just before first use so we never waste an alloc on an
// early-error path.
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
// Note: the file-scope s_blk_buf / s_blk_buf2 / s_bcache_data buffers
// below stay static on purpose — hot path, intentional speed/memory
// tradeoff.  See feedback memory.
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
static uint32_t s_first_data_blk  = 0;   // superblock's s_first_data_block
static uint8_t  s_mounted         = 0;

// Static I/O buffers for misc operations.
static uint8_t s_blk_buf[4096];
static uint8_t s_blk_buf2[4096];

// ── Block cache ───────────────────────────────────────────────────────────
// Direct-mapped cache: 256 slots × 4KB = 1MB BSS.  Block number % BCACHE_SIZE
// determines the slot.  Collisions evict.  Eliminates redundant disk reads
// for indirect blocks (read once per file, hit cache on every subsequent
// data block lookup) and repeated reads of the same data block.

// ── Zero-copy block cache ────────────────────────────────────────────────
// Direct-mapped, 256 slots × 4 KiB = 1 MiB BSS.  Block number % BCACHE_SIZE
// picks the slot.  The hot read path (`bcache_get`) returns a pointer
// straight into the cache — **no memcpy on a hit**.  The caller must
// `bcache_put` the returned ref when done, which drops the per-slot pin
// count so a concurrent writer can eventually refill that slot.
//
// Correctness protocol (pin + double-check):
//
//   Reader:                         Writer (bcache_fill):
//     pin++                           trylock(wlock)
//     smp_mb                          if (pin != 0)        unlock & skip
//     if (tag == blk) return          save old_tag
//     pin--                           tag = INVALID        // poison
//                                     smp_mb
//                                     if (pin != 0) {      // lost the race
//                                         tag = old_tag
//                                         unlock & skip
//                                     }
//                                     memcpy(data, src)
//                                     smp_wmb
//                                     tag = blk
//                                     unlock
//
// Race proof sketch: if reader's (pin++; mb; tag_load) ordering places
// pin++ before writer's (tag_store_INVALID; mb; pin_load), then writer's
// second pin_load sees the reader's pin and bails (tag restored; reader
// sees old tag, either matches or not, no torn data).  If pin++ is after
// writer's mb, then reader's tag_load is after writer's tag_store_INVALID
// by program order through the mb — reader sees INVALID and bails.
// Either way, reader never returns a pointer into a slot that is being
// rewritten.
//
// On slot conflict (writer finds pinned slot holding a different block),
// the writer silently drops the cache update — next reader will miss and
// re-fetch from disk.  Writers (miss-fill, write-back, readahead) are
// rare compared to reads, so the dropped update is harmless.

#define BCACHE_SIZE    256u
#define BCACHE_INVALID 0xFFFFFFFFu

typedef struct {
    spinlock_t wlock;      // writer mutual exclusion (trylock only)
    uint32_t   tag;        // block number, BCACHE_INVALID = empty
    uint32_t   pin;        // atomic: number of active readers holding data[]
} bcache_meta_t;

static uint8_t       s_bcache_data[BCACHE_SIZE][4096];
static bcache_meta_t s_bcache_meta[BCACHE_SIZE];

// Handle returned by bcache_get.  Caller keeps it on the stack and passes
// a pointer to bcache_put when done.  `data` is NULL on I/O error.
typedef struct {
    const uint8_t* data;    // either the pinned cache slot or `scratch`
    uint32_t       blk;
    uint8_t        pinned;  // 1 = release holds a slot pin; 0 = scratch fallback
} bcache_ref_t;

static void bcache_init(void) {
    for (uint32_t i = 0; i < BCACHE_SIZE; i++) {
        spin_lock_init(&s_bcache_meta[i].wlock);
        s_bcache_meta[i].tag = BCACHE_INVALID;
        s_bcache_meta[i].pin = 0;
    }
}

// Publish fresh data into a cache slot.  Silently skips on conflict: if
// the slot is currently pinned by any reader, the update is dropped and
// the caller's data lives only in their buffer.
static void bcache_fill(uint32_t blk, const uint8_t* data, uint32_t len) {
    uint32_t slot = blk % BCACHE_SIZE;
    bcache_meta_t* m = &s_bcache_meta[slot];
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
    __builtin_memcpy(s_bcache_data[slot], data, len);
    smp_wmb();                                    // data stores retire before tag publish
    m->tag = blk;
    spin_unlock(&m->wlock);
}

// Acquire a block.  On a cache hit, `ref.data` points directly into the
// cache (pinned, no copy).  On a miss, the block is read from disk into
// `scratch` and speculatively published into the cache (which may drop
// the update on slot conflict — harmless).
//
// Returns `ref.data == NULL` on I/O error.  Caller must `bcache_put(&ref)`
// on success to drop any held pin.
static bcache_ref_t bcache_get(uint32_t blk, uint8_t* scratch) {
    bcache_ref_t r = { NULL, blk, 0 };
    uint32_t slot = blk % BCACHE_SIZE;
    bcache_meta_t* m = &s_bcache_meta[slot];

    // Fast path — optimistic pin, then verify the tag.  On x86 the
    // atomic_fetch_add is a single `lock xadd` and implies a full barrier.
    __atomic_fetch_add(&m->pin, 1u, __ATOMIC_ACQ_REL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->tag, __ATOMIC_ACQUIRE) == blk) {
        r.data   = s_bcache_data[slot];
        r.pinned = 1;
        return r;
    }
    __atomic_fetch_sub(&m->pin, 1u, __ATOMIC_RELEASE);

    // Slow path — go to disk.
    uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
    if (!ahci_read(lba, scratch, s_sectors_per_blk)) return r;
    bcache_fill(blk, scratch, s_block_size);
    r.data = scratch;
    return r;
}

static void bcache_put(bcache_ref_t* r) {
    if (r->pinned) {
        uint32_t slot = r->blk % BCACHE_SIZE;
        __atomic_fetch_sub(&s_bcache_meta[slot].pin, 1u, __ATOMIC_RELEASE);
        r->pinned = 0;
    }
    r->data = NULL;
}


// ── Inode cache — 2-level radix tree ──────────────────────────────────────
// Key: uint32_t inode number.  Value: ext2_inode_t (cached from disk).
// L0 index = ino >> 8  (top 24 bits collapse into 256 buckets via masking)
// L1 index = ino & 0xFF
// Nodes are allocated on demand via kmalloc; never freed (kernel lifetime).
// write_inode() keeps the cache coherent — no explicit invalidation needed.

#define IRTREE_BITS  8
#define IRTREE_SIZE  (1u << IRTREE_BITS)   // 256
#define IRTREE_MASK  (IRTREE_SIZE - 1u)

typedef struct {
    ext2_inode_t inode;
    uint32_t     ino;
    uint8_t      valid;
} irtree_leaf_t;

typedef struct {
    irtree_leaf_t* leaves[IRTREE_SIZE];
} irtree_l1_t;

static irtree_l1_t* s_irtree[IRTREE_SIZE];  // L0: 256 pointers

static uint8_t irtree_get(uint32_t ino, ext2_inode_t* out) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;
    if (!s_irtree[l0]) return 0;
    irtree_leaf_t* leaf = s_irtree[l0]->leaves[l1];
    if (!leaf || !leaf->valid || leaf->ino != ino) return 0;
    *out = leaf->inode;
    return 1;
}

static void irtree_put(uint32_t ino, const ext2_inode_t* inode) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;

    if (!s_irtree[l0]) {
        s_irtree[l0] = kmalloc(sizeof(irtree_l1_t));
        if (!s_irtree[l0]) return;
        for (uint32_t i = 0; i < IRTREE_SIZE; i++) s_irtree[l0]->leaves[i] = NULL;
    }

    irtree_leaf_t* leaf = s_irtree[l0]->leaves[l1];
    if (!leaf) {
        leaf = kmalloc(sizeof(irtree_leaf_t));
        if (!leaf) return;
        s_irtree[l0]->leaves[l1] = leaf;
    }

    leaf->inode = *inode;
    leaf->ino   = ino;
    leaf->valid = 1;
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
static uint8_t read_bgd(uint32_t g, ext2_bgd_t* out) {
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    bcache_ref_t r = bcache_get(blk_idx, s_blk_buf2);
    if (!r.data) return 0;
    __builtin_memcpy(out, r.data + off, sizeof(ext2_bgd_t));
    bcache_put(&r);
    return 1;
}

// Write BGD for group `g` from `in`.
static uint8_t write_bgd(uint32_t g, const ext2_bgd_t* in) {
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf2)) return 0;

    uint8_t* dst = s_blk_buf2 + off;
    const uint8_t* src = (const uint8_t*)in;
    for (uint32_t i = 0; i < sizeof(ext2_bgd_t); i++) dst[i] = src[i];
    return write_block(blk_idx, s_blk_buf2);
}

// ── Inode I/O ──────────────────────────────────────────────────────────────

static uint8_t read_inode(uint32_t ino, ext2_inode_t* out) {
    if (ino == 0) return 0;
    if (irtree_get(ino, out)) return 1;  // cache hit

    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    // Zero-copy read: pin the cache slot while we copy 128 bytes of
    // inode out.  A 4 KiB block read used to memcpy twice; now the
    // disk-to-bcache copy happens once (on miss) and the bcache-to-out
    // is a single tight struct copy.
    bcache_ref_t r = bcache_get(blk_idx, s_blk_buf);
    if (!r.data) return 0;
    __builtin_memcpy(out, r.data + off, sizeof(ext2_inode_t));
    bcache_put(&r);

    irtree_put(ino, out);  // populate cache
    return 1;
}

static uint8_t write_inode(uint32_t ino, const ext2_inode_t* in) {
    if (ino == 0) return 0;
    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf)) return 0;

    uint8_t* dst = s_blk_buf + off;
    const uint8_t* src = (const uint8_t*)in;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) dst[i] = src[i];
    if (!write_block(blk_idx, s_blk_buf)) return 0;

    irtree_put(ino, in);  // keep cache coherent
    return 1;
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

    // Persist updated inode (size + block pointers).
    fd->inode.i_size   = fd->file_size;
    fd->inode.i_blocks = ((fd->file_size + s_block_size - 1) / s_block_size) * (s_block_size / 512);
    write_inode(fd->ino, &fd->inode);

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

// Allocate a free inode from any group. Returns inode number or 0.
static uint32_t alloc_inode(void) {
    EXT2_SCRATCH(ibm_buf);

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        if (!read_block(bgd.bg_inode_bitmap, ibm_buf)) continue;

        uint32_t bit = bitmap_find_free(ibm_buf, s_inodes_per_grp);
        if (bit == UINT32_MAX) continue;

        bitmap_set(ibm_buf, bit);
        if (!write_block(bgd.bg_inode_bitmap, ibm_buf)) return 0;

        bgd.bg_free_inodes_count--;
        write_bgd(g, &bgd);

        uint32_t ino = g * s_inodes_per_grp + bit + 1;
        return ino;
    }
    return 0; // no free inode
}

// Allocate a free block from any group. Returns block number or 0.
static uint32_t alloc_block(void) {
    EXT2_SCRATCH(bbm_buf);

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        if (!read_block(bgd.bg_block_bitmap, bbm_buf)) continue;

        uint32_t bit = bitmap_find_free(bbm_buf, s_blocks_per_grp);
        if (bit == UINT32_MAX) continue;

        bitmap_set(bbm_buf, bit);
        if (!write_block(bgd.bg_block_bitmap, bbm_buf)) return 0;

        bgd.bg_free_blocks_count--;
        write_bgd(g, &bgd);

        uint32_t blk = g * s_blocks_per_grp + bit + s_first_data_blk;
        return blk;
    }
    return 0;
}

// Free a block.
static void free_block(uint32_t blk) {
    if (!blk) return;
    EXT2_SCRATCH_VOID(fbb_buf);

    // Determine which group this block belongs to.
    uint32_t rel = (blk >= s_first_data_blk) ? (blk - s_first_data_blk) : blk;
    uint32_t g   = rel / s_blocks_per_grp;
    uint32_t bit = rel % s_blocks_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return;
    if (!read_block(bgd.bg_block_bitmap, fbb_buf)) return;

    if (bitmap_test(fbb_buf, bit)) {
        bitmap_clear(fbb_buf, bit);
        write_block(bgd.bg_block_bitmap, fbb_buf);
        bgd.bg_free_blocks_count++;
        write_bgd(g, &bgd);
    }
}

// Free an inode.
static void free_inode_num(uint32_t ino) {
    if (!ino) return;
    EXT2_SCRATCH_VOID(fib_buf);

    uint32_t g   = (ino - 1) / s_inodes_per_grp;
    uint32_t bit = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return;
    if (!read_block(bgd.bg_inode_bitmap, fib_buf)) return;

    if (bitmap_test(fib_buf, bit)) {
        bitmap_clear(fib_buf, bit);
        write_block(bgd.bg_inode_bitmap, fib_buf);
        bgd.bg_free_inodes_count++;
        write_bgd(g, &bgd);
    }
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
    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino_num, &dir_inode)) return 0;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return 0;

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
            if (!blk) return 0;
            zero_block(blk);
            if (!inode_set_block(&dir_inode, blk_idx, blk)) {
                free_block(blk);
                return 0;
            }
            dir_inode.i_size   += s_block_size;
            dir_inode.i_blocks += s_block_size / 512;
            new_blk = 1;
        } else {
            blk = inode_get_block(&dir_inode, blk_idx);
            if (!blk) break;
        }

        if (!read_block(blk, dae_buf)) return 0;

        if (new_blk) {
            // Write entry as the only entry in this fresh block.
            ext2_dirent_t* de = (ext2_dirent_t*)dae_buf;
            de->inode     = child_ino;
            de->rec_len   = (uint16_t)s_block_size;
            de->name_len  = (uint8_t)name_len;
            de->file_type = file_type;
            for (uint32_t i = 0; i < name_len; i++) de->name[i] = name[i];
            if (!write_block(blk, dae_buf)) return 0;
            write_inode(dir_ino_num, &dir_inode);
            return 1;
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
                if (!write_block(blk, dae_buf)) return 0;
                write_inode(dir_ino_num, &dir_inode);
                return 1;
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
                if (!write_block(blk, dae_buf)) return 0;
                write_inode(dir_ino_num, &dir_inode);
                return 1;
            }

            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;

        blk_idx++;
    }

    return 0;
}

// Remove a directory entry by name from the directory `dir_ino_num`.
// Marks the entry's inode field as 0 (deleted). Returns 1 on success, 0 if not found.
static uint8_t dir_remove_entry(uint32_t dir_ino_num, const char* name) {
    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino_num, &dir_inode)) return 0;

    uint32_t name_len = str_len(name);
    uint32_t bytes_left = dir_inode.i_size;
    uint32_t blk_idx = 0;

    EXT2_SCRATCH_DECL(dre_buf);  // alloc lazily — empty dir = no work
    while (bytes_left > 0) {
        uint32_t blk = inode_get_block(&dir_inode, blk_idx);
        blk_idx++;
        if (!blk) break;

        if (!dre_buf) EXT2_SCRATCH_ALLOC(dre_buf);
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
                return 1;
            }
            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }
    return 0;
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
        write_inode(existing_ino, &old_inode);
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
    write_inode(new_ino, &new_inode);

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
    write_inode(new_ino, &new_inode);

    // Add to parent directory.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_DIR)) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }

    // Increment parent's links_count for ".."
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode)) {
        parent_inode.i_links_count++;
        write_inode(parent_ino, &parent_inode);
    }

    // Increment used_dirs_count in parent's BGD.
    uint32_t g = (new_ino - 1) / s_inodes_per_grp;
    ext2_bgd_t bgd;
    if (read_bgd(g, &bgd)) {
        bgd.bg_used_dirs_count++;
        write_bgd(g, &bgd);
    }

    return 1;
}

// ── ext2_unlink ───────────────────────────────────────────────────────────
// Remove a regular file at `path`.  Returns 1 on success, 0 on failure.
int ext2_unlink(const char* path) {
    if (!s_mounted || !path) return 0;

    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;

    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return 0;

    // Refuse to unlink directories.
    if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR) return 0;

    char ul_parent[256];
    const char* basename = path_split(path, ul_parent);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(ul_parent);
    if (!parent_ino) return 0;

    // Remove directory entry first.
    if (!dir_remove_entry(parent_ino, basename)) return 0;

    // Decrement link count; only free inode/data when it reaches 0.
    if (inode.i_links_count > 0) inode.i_links_count--;
    if (inode.i_links_count == 0) {
        free_inode_blocks(&inode);
        inode.i_dtime = 1;
        write_inode(ino, &inode);
        free_inode_num(ino);
    } else {
        write_inode(ino, &inode);
    }

    return 1;
}

// ── ext2_rename ───────────────────────────────────────────────────────────
// Move/rename `src` to `dst`.  Returns 1 on success, 0 on failure.
// If `dst` already exists as a regular file it is removed first.
int ext2_rename(const char* src, const char* dst) {
    if (!s_mounted || !src || !dst) return 0;

    uint32_t src_ino = path_to_inode(src);
    if (!src_ino) return 0;

    ext2_inode_t src_inode;
    if (!read_inode(src_ino, &src_inode)) return 0;

    uint8_t is_dir = ((src_inode.i_mode & 0xF000) == EXT2_S_IFDIR);

    // Resolve src parent/basename.
    char rn_src_parent[256];
    const char* src_base = path_split(src, rn_src_parent);
    if (!src_base || src_base[0] == '\0') return 0;
    uint32_t src_parent_ino = path_to_inode(rn_src_parent);
    if (!src_parent_ino) return 0;

    // Resolve dst parent/basename.
    char rn_dst_parent[256];
    const char* dst_base = path_split(dst, rn_dst_parent);
    if (!dst_base || dst_base[0] == '\0') return 0;
    uint32_t dst_parent_ino = path_to_inode(rn_dst_parent);
    if (!dst_parent_ino) return 0;

    // If dst exists as a regular file, unlink it.
    uint32_t dst_ino = path_to_inode(dst);
    if (dst_ino) {
        ext2_inode_t dst_inode;
        if (!read_inode(dst_ino, &dst_inode)) return 0;
        if ((dst_inode.i_mode & 0xF000) == EXT2_S_IFDIR) return 0; // refuse dir collision
        dir_remove_entry(dst_parent_ino, dst_base);
        free_inode_blocks(&dst_inode);
        dst_inode.i_links_count = 0;
        write_inode(dst_ino, &dst_inode);
        free_inode_num(dst_ino);
    }

    // Add new directory entry pointing at the same inode.
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (!dir_add_entry(dst_parent_ino, dst_base, src_ino, ft)) return 0;

    // Remove old directory entry.
    dir_remove_entry(src_parent_ino, src_base);

    return 1;
}
