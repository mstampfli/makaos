#pragma once
#include "common.h"

#define MAX_RAM_BYTES (64ULL * 1024 * 1024 * 1024)
#define MAX_FRAMES (MAX_RAM_BYTES / PAGE_SIZE)

#define PMM_INVALID_FRAME UINT64_MAX
#define PMM_INVALID_ADDR  UINT64_MAX

#define PMM_RESERVED_FRAMES ((4 * 1024 * 1024) >> PAGE_SHIFT)

#define MAX_ORDER 32

#define SLAB_MAX_ORDER 3
#define SLAB_WASTE_FRACTION 16
#define SLAB_MIN_OBJECTS 8
#define INVALID_ORDER ((uint8_t)255)

typedef struct mem_survey_t {
  uint64_t hole;
  uint64_t ceiling;
} mem_survey_t;

typedef struct slab_cache_t slab_cache_t;

// Mini rcu_head compatible with the definition in rcu.h — kept here
// so pmm.h doesn't drag rcu.h into every translation unit that reads
// slab state.  Layout MUST match rcu_head_t exactly (first fields:
// next, func, data).  static_assert in rcu.c validates the shape.
typedef struct pmm_rcu_head {
    struct pmm_rcu_head* next;
    void               (*func)(void* data);
    void*                data;
} pmm_rcu_head_t;

typedef struct slab_header_t {
  slab_cache_t* cache;     // owning cache
  phys_addr_t   slab_phys; // base physical address of the slab
  void*         freelist;  // intrusive free-slot stack
  uint32_t      inuse;     // number of allocated objects

  struct slab_header_t* prev;
  struct slab_header_t* next;

  uint8_t       on_list;   // which list the page is currently on; see SLAB_LIST_*

  // ── Cross-CPU remote-free queue (Phase 4G / 5A) ────────────────
  // Treiber stack; single-CAS push from any CPU.  Drained under
  // g_pmm_lock at every slow-path transition (see
  // drain_remote_into_freelist_locked in pmm.c).
  void*         remote_free;

  // ── RCU callback head (Phase 5B) ──────────────────────────────
  // Embedded so pmm_slab_destroy_locked can defer buddy return via
  // call_rcu_head without allocating from inside g_pmm_lock.
  // Unused when SLAB_TYPESAFE_BY_RCU isn't set — cheap memory.
  pmm_rcu_head_t rcu_head;
} slab_header_t;

// Which list a page is currently on (drives the page state machine —
// see kernel/mm/pmm.c "page state machine" comment block).
enum slab_list_kind {
    SLAB_LIST_NONE       = 0,  // freshly allocated / about to be freed to buddy
    SLAB_LIST_PARTIAL    = 1,  // on cache->partial — has free slots, no CPU bound
    SLAB_LIST_FULL       = 2,  // on cache->full    — no free slots, no CPU bound
    SLAB_LIST_EMPTY      = 3,  // on cache->empty   — fully free, shrinker reclaim
    SLAB_LIST_CPU_PART   = 4,  // on this_cpu()->partial_head[cls]
    SLAB_LIST_CPU_ACTIVE = 5,  // == this_cpu()->cpu_slab[cls] — fast-path target
    SLAB_LIST_RCU_DEFER  = 6,  // unlinked, waiting for RCU grace period (Phase 5B)
};

// ── Phase 5B: slab cache flags ─────────────────────────────────────────
// Bitfield passed to pmm_slab_cache_init.  Compile-time constants.
//
// SLAB_TYPESAFE_BY_RCU: when the cache shrinks and would return a
// whole slab page to the buddy, that return is deferred via call_rcu
// until every CPU has passed through a quiescent state.  Readers
// holding rcu_read_lock can safely dereference pointers into objects
// on this cache even after the object has been freed — the memory
// is still slab-typed (pmm_is_slab_ptr still returns true) and any
// concurrent alloc from the same page yields a pointer to the same
// type.  Callers using this flag MUST embed a version counter /
// seqlock in their objects so readers can detect instance reuse.
// Typical users: dentry, inode, SIGACTION tables — anywhere
// traversal needs to avoid per-object refcounting.
typedef enum {
    SLAB_TYPESAFE_BY_RCU = 1u << 0,
    // Future flags reserved: SLAB_HWCACHE_ALIGN, SLAB_ACCOUNT, ...
} slab_flags_t;

// partial/full/empty are pointers to slab_header_t nodes embedded in
// slab pages.  Kept as void* to avoid exposing slab_header_t in the
// public header.
typedef struct slab_cache_t {
  size_t  slot_size;    // size of one object/slot in bytes
  void*   partial;      // pages with free slots, no CPU bound
  void*   full;         // pages with no free slots, no CPU bound
  void*   empty;        // pages fully free; awaiting shrinker (Phase 4F)
  void*   rcu_pending;  // Phase 5B: typesafe pages awaiting grace period
                        // before buddy release — drained by slab shrinker
                        // after synchronize_rcu().  Only used when
                        // cache->flags & SLAB_TYPESAFE_BY_RCU.
  uint8_t  slab_order;   // buddy order used to allocate slabs (0 = 1 page)
  uint8_t  class_idx;    // index into per-CPU cpu_slot[] / cpu_slab[]
                         // for kheap-managed caches; UINT8_MAX otherwise.
  uint8_t  flags;        // slab_flags_t bitmask (Phase 5B)
  uint8_t  _pad;
  uint16_t obj_per_slab; // # objects each slab page holds, computed at
                         // init from slab_order + slot_size.  Used by
                         // slab_pcpu promote/demote to set h->inuse
                         // correctly (the lockless fast path doesn't
                         // touch inuse, so the slow path's FULL ↔
                         // PARTIAL ↔ EMPTY transitions need the
                         // baseline established at promote time).

  uint16_t cpu_partial_cap;  // Phase 5C: max pages per CPU partial list
                             // for this cache.  Scales inversely with
                             // obj_per_slab so hot small-object caches
                             // (kmalloc-64 etc) accumulate more pages
                             // and amortise g_pmm_lock across more
                             // allocations.  Computed at cache init.

  // Singly-linked list of all caches, threaded through `cache_next`.
  // Used by the slab shrinker (Phase 4F) and the per-cache iterator
  // for stats / debug.  Linked at pmm_slab_cache_init time.
  struct slab_cache_t* cache_next;
} slab_cache_t;

// ── Per-frame slab page state (Phase 4) ────────────────────────────────
// Returns one of the SLAB_LIST_* values above for the page that backs
// the given physical / virtual address.  Used by the per-CPU magazine
// layer to decide whether a free can hit the fast path or must enter
// the cross-CPU remote-free queue.  Returns SLAB_LIST_NONE if the
// frame is not a slab page.
uint8_t       pmm_slab_state_get(phys_addr_t phys);
slab_header_t* pmm_slab_header_of(void* ptr);
slab_cache_t*  pmm_slab_cache_of(void* ptr);

// Iterate every registered slab cache (linked at pmm_slab_cache_init
// time).  Returns the first cache; cache->cache_next walks the rest.
slab_cache_t* pmm_slab_cache_first(void);

// ── Phase 4 helpers used by slab_pcpu.c (per-CPU magazine layer) ──────
// Each "grab" pops one page off the named cache list and returns it
// unlinked, with on_list set to SLAB_LIST_NONE.  The caller is
// expected to install it as a CPU-active page (or push it onto its
// own per-CPU partial list).  Returns NULL if the list is empty.
slab_header_t* pmm_slab_grab_partial(slab_cache_t* cache);
slab_header_t* pmm_slab_grab_empty(slab_cache_t* cache);
slab_header_t* pmm_slab_grow(slab_cache_t* cache);   // alloc fresh from buddy

// Push a CPU-displaced page onto the named cache list.  Used when a CPU
// retires its current cpu_slab[cls] and there's no room on the per-CPU
// partial list.  Sets on_list accordingly.  Caller does NOT hold any
// lock; this function takes g_pmm_lock internally.
void pmm_slab_park_partial(slab_cache_t* cache, slab_header_t* h);
void pmm_slab_park_full(slab_cache_t* cache, slab_header_t* h);
void pmm_slab_park_empty(slab_cache_t* cache, slab_header_t* h);

// Phase 4G: demote a CPU_ACTIVE page.  Takes g_pmm_lock.  Reads any
// frees that landed on h->freelist via pmm_slab_free_locked while h
// was CPU_ACTIVE, returning that chain in *out_fl (caller can re-use
// it on cpu_slot).  When *out_fl is NULL (no frees waiting) the page
// is parked onto cache->full; the page is otherwise left CPU_ACTIVE
// (caller keeps it bound).  Returns 1 if the page was kept active,
// 0 if it was parked.
int pmm_slab_demote_or_keep(slab_cache_t* cache, slab_header_t* h, void** out_fl);

// Phase 5C: batched grab from cache->partial.  Pops up to `want`
// pages off cache->partial in ONE g_pmm_lock acquire; writes them
// into out[], returns the number actually removed.  All returned
// pages are drained of their remote_free (as in pmm_slab_grab_partial)
// and have on_list = SLAB_LIST_NONE.  The caller typically promotes
// out[0] to CPU_ACTIVE and stashes out[1..] on its per-CPU partial
// list.  Returns 0 if cache->partial was empty.
uint32_t pmm_slab_grab_partial_batch(slab_cache_t* cache,
                                      slab_header_t** out, uint32_t want);

// Phase 5C: batched park onto cache->partial.  Pushes `count` pages
// onto cache->partial in ONE g_pmm_lock acquire.  Each page's
// on_list is set to SLAB_LIST_PARTIAL.  Drain of remote_free is the
// caller's responsibility (they've just owned these pages; no
// cross-CPU frees could have landed yet).
void pmm_slab_park_partial_batch(slab_cache_t* cache,
                                  slab_header_t** pages, uint32_t count);

// ── Phase 4E/4F: raw g_pmm_lock access for batched allocators ────────
// pcp.c takes the PMM lock for a whole batch of order-0 alloc/free so
// the hot-path lock-contention collapses to ~1 in 32 allocs.  The
// shrinker (4F) uses the same pair to wrap pmm_slab_shrink_all_locked.
void        pmm_pcp_lock(uint64_t* flags_out);
void        pmm_pcp_unlock(uint64_t flags);
phys_addr_t pmm_buddy_alloc_locked_for_pcp(uint8_t order);
void        pmm_buddy_free_locked_for_pcp(phys_addr_t addr, uint8_t order);

// Drain every cache's empty-list back to buddy.  Caller MUST hold
// g_pmm_lock.  Used by pmm_buddy_alloc OOM path and the shrinker.
void        pmm_slab_shrink_all_locked(void);

// Phase 5D: bounded drain — reclaim up to `max_per_cache` pages
// from EACH cache's empty-list.  Drains from the tail of the list
// (oldest pages first — cache->empty is head-push, so tail is LRU).
// Caller MUST hold g_pmm_lock.  Used by the pressure-aware shrinker
// when memory isn't critical enough to warrant a full reclaim.
void        pmm_slab_shrink_bounded_locked(uint32_t max_per_cache);

// Phase 5D: free-memory ratio in basis points (per 10000).  Used by
// the shrinker to gate scan frequency and scan fraction.  Lockless
// read of atomic counters — may race with a concurrent alloc/free
// but the read is intact.  Returns 10000 if total_frames == 0
// (uninitialised).
uint32_t    pmm_free_ratio_bp(void);

phys_addr_t pmm_highest_address_get(void);
uint64_t    pmm_total_frames_get(void);
uint64_t    pmm_free_pages_get(void);

// ── Per-frame refcount (Copy-on-Write) ────────────────────────────────────
// Every frame starts at refcount 1 on alloc.  CoW fork increments it.
// pmm_ref_dec frees the frame when refcount drops to 0.
void     pmm_ref_inc(phys_addr_t addr);
void     pmm_ref_dec(phys_addr_t addr);   // frees frame when rc hits 0
uint32_t pmm_ref_get(phys_addr_t addr);

// ── Per-frame pin count (DMA safety) ─────────────────────────────────────
// Pinned frames must not be CoW-shared — fork deep-copies them instead.
void     pmm_pin(phys_addr_t addr);
void     pmm_unpin(phys_addr_t addr);
uint16_t pmm_pin_get(phys_addr_t addr);

mem_survey_t pmm_mem_survey(e820_entry_t* map, uint32_t count);

uint8_t calculate_max_order(uintptr_t addr, uintptr_t end);
uint8_t calculate_slab_order(size_t slot_size);

void pmm_buddy_init_from_map(e820_entry_t* map, uint32_t count);
phys_addr_t pmm_buddy_alloc(uint8_t order);
void pmm_buddy_free(phys_addr_t addr, uint8_t order);

// Initialise a slab cache.  `flags` is a bitmask of slab_flags_t;
// pass 0 for the default (immediate page reclaim on shrink/destroy).
void pmm_slab_cache_init(slab_cache_t* cache, size_t slot_size, uint8_t flags);
void* pmm_slab_alloc(slab_cache_t* cache);
void pmm_slab_free(void* ptr);
uint8_t pmm_is_slab_ptr(void* ptr);
