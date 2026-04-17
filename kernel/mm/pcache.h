#pragma once
#include "common.h"
#include "pmm.h"

// ── Page Cache ─────────────────────────────────────────────────────────────
//
// Maps (inode_number, page_index) → phys_addr_t so that warm exec paths
// serve pages from RAM instead of re-reading them from disk.
//
// Design:
//   - 1024-bucket hash table, each bucket protected by its own spinlock.
//     No global lock: concurrent fault handlers on different CPUs operate
//     on different buckets independently.
//   - Cache entries are kmalloc'd nodes in per-bucket intrusive linked lists.
//   - PMM refcounting: the cache holds ONE PMM ref per cached frame.
//     Callers add their own ref (pmm_ref_inc) for each PTE that maps the frame.
//     When a process exits, vmm_free_user_ex pmm_ref_dec's PTE refs; the
//     cache ref keeps the frame alive for the next exec.
//
// Sharing policy:
//   - Read-only segments (text/rodata): shared directly — PTE maps the
//     cached frame with RO flags; pmm_ref_inc adds the PTE ref.
//   - Writable segments (data/BSS): copy-on-exec — private frame allocated,
//     memcpy'd from cached clean copy, mapped with RW flags.  The cache
//     always retains the clean on-disk version.
//
// The cache currently grows without eviction (kernel has ample RAM for
// the small number of binaries typically executed).  pcache_evict_inode
// flushes all entries for one inode — call it on file write or unlink to
// prevent stale reads.

void        pcache_init(void);

// Look up (ino, pg_idx).  Returns the cached frame address if found, or
// PMM_INVALID_ADDR on miss.  The cache's own ref keeps the frame alive;
// caller MUST pmm_ref_inc() before mapping the frame into a PTE.
// ino == 0 always misses (non-ext2 files are not cacheable).
phys_addr_t pcache_get(uint32_t ino, uint32_t pg_idx);

// Offer a freshly-read frame to the cache.  The cache claims ownership of
// the frame's alloc ref (rc == 1 from pmm_buddy_alloc).
//
// Returns the frame that is now in the cache:
//   - If this call inserted it: returns `frame` (same as input).
//   - If a racing CPU already inserted the same (ino, pg_idx): frees `frame`
//     (pmm_ref_dec) and returns the existing frame.
//   - On OOM (can't alloc cache entry): returns PMM_INVALID_ADDR and leaves
//     `frame` untouched (rc == 1, caller owns it as a PTE ref).
//
// After a non-INVALID return, call pmm_ref_inc(result) for your PTE.
// After PMM_INVALID_ADDR, use `frame` directly as the sole PTE ref (no inc).
phys_addr_t pcache_insert(uint32_t ino, uint32_t pg_idx, phys_addr_t frame);

// Evict all entries for `ino`.  pmm_ref_dec's the cached frame for each
// evicted entry.  Call on file write or unlink so subsequent faults re-read
// from disk.
void        pcache_evict_inode(uint32_t ino);
