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
//     No global lock on the hot path: concurrent fault handlers on different
//     CPUs operate on different buckets independently.
//   - CLOCK eviction: all entries form a circular doubly-linked list.
//     A background reclaim kthread sweeps the CLOCK hand, clearing accessed
//     bits and evicting cold pages to maintain a free-page buffer.
//   - PMM refcounting: the cache holds ONE PMM ref per cached frame.
//     Callers add their own ref (pmm_ref_inc) for each PTE that maps it.
//
// Reclaim policy:
//   - Target: cache may use up to 80% of (free_pages + cached_pages).
//   - The remaining 20% stays genuinely free for instant allocation.
//   - The reclaim kthread wakes when cache exceeds target and evicts
//     CLOCK victims until the target is restored.  Proactive — allocators
//     never block on reclaim.

void        pcache_init(void);

// Look up (ino, pg_idx).  Returns cached frame or PMM_INVALID_ADDR on miss.
// Sets the CLOCK accessed bit on hit.
// Caller MUST pmm_ref_inc() the returned frame before mapping into a PTE.
phys_addr_t pcache_get(uint32_t ino, uint32_t pg_idx);

// Insert a freshly-read frame.  Cache claims the alloc ref (rc==1).
// Returns the cached frame (may differ from input on race).
// PMM_INVALID_ADDR on OOM.
phys_addr_t pcache_insert(uint32_t ino, uint32_t pg_idx, phys_addr_t frame);

// Evict all entries for an inode (call on write/unlink).
void        pcache_evict_inode(uint32_t ino);

// Current number of cached pages.
uint64_t    pcache_count_get(void);

// Spawn the background reclaim kthread.  Call after sched_init().
void        pcache_start_reclaim_thread(void);
