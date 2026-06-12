#include "pmm.h"
#include "common.h"
#include "smp.h"
#include "rcu.h"      // Phase 5B: call_rcu for SLAB_TYPESAFE_BY_RCU
#include "cpu.h"      // Phase 5C: SLAB_PCPU_PARTIAL_DEPTH cap

// ── SMP correctness — global PMM lock ───────────────────────────────────
//
// This is Phase 4's placeholder for SMP safety while the per-CPU
// magazine/pageset redesign is deferred (see docs/PHASE4_REDESIGN.md).
// Every public entry point below acquires g_pmm_lock (IRQ-safe variant
// because pmm_ref_dec can be reached from CoW page-fault paths that may
// run with some IRQs masked) for the duration of its critical section.
//
// Consequences:
//   - Correct under SMP: two CPUs cannot race on the buddy free lists,
//     the slab cache heads, the per-frame refcount, or the slab tracker
//     array.
//   - Slow under contention: every alloc/free serializes on one global
//     lock.  On a single CPU (today) the lock is always uncontended so
//     the cost is ~20 cycles per call (one lock cmpxchg + pushfq/popfq).
//   - To be replaced in Phase 4 proper with per-CPU magazines and a
//     per-class depot lock.
//
// The _locked suffix on internal helpers indicates "caller holds
// g_pmm_lock".  Never call a non-_locked public entry from inside a
// critical section — it would self-deadlock.
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

// Forward declarations for _locked internals so init code can call them
// without going through the public lock-taking wrappers.
static phys_addr_t pmm_buddy_alloc_locked(uint8_t order);
static void        pmm_buddy_free_locked(phys_addr_t addr, uint8_t order);
static void        pmm_count_free_pages(void);
static void        pmm_slab_destroy_locked(slab_cache_t* cache, slab_header_t* h);
static inline void drain_remote_into_freelist_locked(slab_header_t* h);

static phys_addr_t g_phys_ceiling = 0;
static uint64_t    g_total_frames = 0;
static volatile uint64_t g_free_pages = 0;  // approximate free page count

// Free List Overlay: lives in the first 16 bytes of a free frame.
typedef struct free_block {
  struct free_block* next;
  struct free_block* prev;
} free_block_t;

// Free Lists: one head per order.
static free_block_t* g_free_lists[MAX_ORDER + 1];

// Coalesce Bitmaps: 1 bit per buddy pair.
// Bit = 1 (Different state), Bit = 0 (Same state: both free or both alloc).
static uint64_t* g_buddy_coalesce[MAX_ORDER + 1];

static uint64_t g_buddy_blocks[MAX_ORDER + 1];          // number of blocks at each order
static uint64_t g_buddy_words_coalesce[MAX_ORDER + 1];

// ── Per-frame refcount + pin count (CoW / DMA) ──────────────────────────
// Indexed by frame number (phys >> PAGE_SHIFT).
// refcount: starts at 1 on alloc, incremented by CoW fork, decremented by
//           vmm_free_user / CoW break.  Frame freed when rc hits 0.
// pincount: non-zero while DMA is in flight to this frame.  Fork must
//           deep-copy pinned frames instead of CoW-sharing them.
static uint32_t* g_frame_refcount;   // [frame_index] → reference count
static uint16_t* g_frame_pincount;   // [frame_index] → pin count

// Slab trackers
static slab_cache_t** g_slab_trackers;   // [frame_index] -> owning cache or NULL
static void** g_slab_heads;      // [frame_index] -> (void*)head_frame_index

// ── Singly-linked list of registered caches (Phase 4) ─────────────────
// Linked at pmm_slab_cache_init() time.  The shrinker (4F) and stats
// dump (4H) walk this.  Mutated only at boot when caches are
// registered, then read-only — no synchronisation needed.
static slab_cache_t* g_slab_cache_head = NULL;

static inline uint64_t align_down(uint64_t addr) {
  return (addr & ~(PAGE_SIZE - 1));
}

static inline uint64_t align_up(uint64_t addr) {
  return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline void zero(virt_addr_t addr, uint64_t amount_bytes) {
  uint8_t* ptr = (uint8_t*)addr;
  for (volatile uint64_t i = 0; i < amount_bytes; i++) {
    ptr[i] = 0;
  }
}

static inline virt_addr_t phys_to_virt(phys_addr_t phys) {
  return (virt_addr_t)(phys + HHDM_OFFSET);
}

static inline phys_addr_t virt_to_phys(virt_addr_t virt) {
  return (phys_addr_t)(virt - HHDM_OFFSET);
}

static inline uint64_t bytes_for_bits(uint64_t bit_count) {
  return ((bit_count + 63) >> 6) * sizeof(uint64_t);
}

static inline uint64_t word_index(uint64_t bit_index) {
  return (bit_index >> 6);
}

static inline uint64_t bit_mask(uint64_t bit_index) {
  return (1ULL << (bit_index & 63));
}

static inline int bit_test(uint64_t* words, uint64_t bit_index) {
  return (words[word_index(bit_index)] & bit_mask(bit_index)) != 0;
}

static inline void bit_toggle(uint64_t* words, uint64_t bit_index) {
  words[word_index(bit_index)] ^= bit_mask(bit_index);
}

// Internal list helpers handling HHDM conversion
static inline void free_list_push(uint8_t order, phys_addr_t phys) {
  free_block_t* node = (free_block_t*)phys_to_virt(phys);
  free_block_t** head = &g_free_lists[order];

  node->prev = NULL;
  node->next = *head;

  if (*head) {
    (*head)->prev = node;
  }
  *head = node;
}

static inline void free_list_remove(uint8_t order, phys_addr_t phys) {
  free_block_t* node = (free_block_t*)phys_to_virt(phys);
  free_block_t** head = &g_free_lists[order];

  if (node->prev) node->prev->next = node->next;
  else *head = node->next;

  if (node->next) node->next->prev = node->prev;

  node->prev = NULL;
  node->next = NULL;
}

static inline phys_addr_t free_list_pop(uint8_t order) {
  free_block_t* node = g_free_lists[order];
  if (!node) return PMM_INVALID_ADDR;

  g_free_lists[order] = node->next;
  if (g_free_lists[order]) {
    g_free_lists[order]->prev = NULL;
  }

  node->next = NULL;
  node->prev = NULL;

  return virt_to_phys((virt_addr_t)node);
}

static inline uint64_t order_to_pages(uint8_t order) {
  return (1ULL << order);
}

static inline uint64_t order_to_bytes(uint8_t order) {
  return (PAGE_SIZE << order);
}

mem_survey_t pmm_mem_survey(e820_entry_t* map, uint32_t count) {
  uint64_t hole_idx_largest_base = PMM_INVALID_FRAME;
  uint64_t hole_size_largest = 0; // in frame indices

  uintptr_t kernel_start_idx = align_down(KERNEL_BASE_PHYS) >> PAGE_SHIFT;
  uintptr_t kernel_end_idx = align_up(KERNEL_BASE_PHYS + LOADER_RESERVED_SIZE) >> PAGE_SHIFT;

  for (uint32_t i = 0; i < count; i++) {
    e820_entry_t entry = map[i];

    if (entry.type != 1) continue;

    uint64_t end = entry.base + entry.length;
    if (end > g_phys_ceiling) {
      g_phys_ceiling = end;
    }

    uint64_t start = align_up(entry.base);
    end = align_down(entry.base + entry.length);

    uint64_t start_idx = start >> PAGE_SHIFT;
    uint64_t end_idx = end >> PAGE_SHIFT;

    if (end_idx <= PMM_RESERVED_FRAMES) continue;
    if (start_idx < PMM_RESERVED_FRAMES) start_idx = PMM_RESERVED_FRAMES;

    if (start_idx >= kernel_start_idx && end_idx <= kernel_end_idx) continue;

    if (start_idx >= MAX_FRAMES) continue;
    if (end_idx > MAX_FRAMES) end_idx = MAX_FRAMES;
    if (start_idx >= end_idx) continue;

    uint64_t hole_size;

    if (start_idx < kernel_end_idx && end_idx > kernel_start_idx) {
      if (start_idx >= kernel_start_idx) goto skip_before;

      hole_size = kernel_start_idx - start_idx;
      if (hole_size <= hole_size_largest) goto skip_before;

      hole_size_largest = hole_size;
      hole_idx_largest_base = start_idx;

    skip_before:
      if (end_idx <= kernel_end_idx) continue;

      hole_size = end_idx - kernel_end_idx;
      if (hole_size <= hole_size_largest) continue;

      hole_size_largest = hole_size;
      hole_idx_largest_base = kernel_end_idx;

      continue;
    }

    hole_size = end_idx - start_idx;
    if (hole_size > hole_size_largest) {
      hole_size_largest = hole_size;
      hole_idx_largest_base = start_idx;
    }
  }

  if (hole_idx_largest_base == PMM_INVALID_FRAME) {
    return (mem_survey_t){ .hole = PMM_INVALID_ADDR, .ceiling = PMM_INVALID_ADDR };
  }

  return (mem_survey_t){ .hole = (phys_addr_t)(hole_idx_largest_base << PAGE_SHIFT), .ceiling = g_phys_ceiling };
}

uint8_t calculate_max_order(uintptr_t addr, uintptr_t end) {
  uint64_t frame_index = addr >> PAGE_SHIFT;

  uint8_t align_order = (frame_index == 0) ? MAX_ORDER : (uint8_t)__builtin_ctzll(frame_index);

  uint64_t pages = (end - addr) >> PAGE_SHIFT;
  uint8_t range_order = (pages == 0) ? 0 : (uint8_t)(63 - __builtin_clzll(pages));

  uint8_t current_order = align_order;
  if (current_order > range_order) current_order = range_order;
  if (current_order > MAX_ORDER) current_order = MAX_ORDER;

  return current_order;
}

static inline int block_overlaps_forbidden(uint64_t frame_index,
                                           uint8_t current_order,
                                           uint64_t meta_frame_start_idx,
                                           uint64_t meta_frame_end_idx,
                                           uint64_t kernel_start_idx,
                                           uint64_t kernel_end_idx) {
  uint64_t block_pages = order_to_pages(current_order);
  uint64_t block_end   = frame_index + block_pages;

  if (frame_index < PMM_RESERVED_FRAMES) return 1;
  if (frame_index < PMM_RESERVED_FRAMES && block_end > PMM_RESERVED_FRAMES) return 1;

  if (frame_index < meta_frame_end_idx && block_end > meta_frame_start_idx) return 1;

  if (frame_index < kernel_end_idx && block_end > kernel_start_idx) return 1;

  if (block_end > g_total_frames) return 1;

  return 0;
}

void pmm_buddy_init_from_map(e820_entry_t* map, uint32_t count) {
  mem_survey_t survey = pmm_mem_survey(map, count);
  if (survey.hole == PMM_INVALID_ADDR || survey.ceiling == PMM_INVALID_ADDR) {
    return;
  }

  g_total_frames = (survey.ceiling >> PAGE_SHIFT);
  if (g_total_frames > MAX_FRAMES) g_total_frames = MAX_FRAMES;

  // Metadata:
  // coalesce bitmaps for all orders (0.5 bits per frame total)
  // slab_trackers + slab_heads
  uint64_t meta_size_bytes = 0;

  for (uint8_t current_order = 0; current_order <= MAX_ORDER; current_order++) {
    uint64_t blocks = (g_total_frames + order_to_pages(current_order) - 1) >> current_order;
    g_buddy_blocks[current_order] = blocks;

    // 1 bit per buddy pair -> blocks / 2
    uint64_t coalesce_bits = (blocks + 1) >> 1;
    g_buddy_words_coalesce[current_order] = (coalesce_bits + 63) >> 6;
    meta_size_bytes += bytes_for_bits(coalesce_bits);
  }

  meta_size_bytes += g_total_frames * sizeof(uint32_t);       // g_frame_refcount
  meta_size_bytes += g_total_frames * sizeof(uint16_t);       // g_frame_pincount
  meta_size_bytes += g_total_frames * sizeof(slab_cache_t*);
  meta_size_bytes += g_total_frames * sizeof(void*);

  uint64_t meta_frames = (align_up(meta_size_bytes) >> PAGE_SHIFT);

  // ensure the chosen hole fits meta_frames
  {
    uint64_t hole_start = align_up(survey.hole);
    uint64_t hole_end = hole_start;
    uint64_t hole_size_frames = 0;

    for (uint32_t i = 0; i < count; i++) {
      if (map[i].type != 1) continue;

      uint64_t start = align_up(map[i].base);
      uint64_t end   = align_down(map[i].base + map[i].length);

      if (hole_start < start || hole_start >= end) continue;

      hole_end = end;
      hole_size_frames = (hole_end - hole_start) >> PAGE_SHIFT;
      break;
    }

    if (hole_size_frames < meta_frames) {
      return;
    }
  }

  phys_addr_t meta_phys = survey.hole;
  virt_addr_t meta_virt = phys_to_virt(meta_phys);

  uint64_t meta_frame_start_idx = (meta_phys >> PAGE_SHIFT);
  uint64_t meta_frame_end_idx   = meta_frame_start_idx + meta_frames;

  uint64_t kernel_start_idx = (align_down(KERNEL_BASE_PHYS) >> PAGE_SHIFT);
  uint64_t kernel_end_idx   = (align_up(KERNEL_BASE_PHYS + LOADER_RESERVED_SIZE) >> PAGE_SHIFT);

  // carve: [coalesce]... [slab_trackers][slab_heads]
  uint64_t off = 0;

  for (uint8_t current_order = 0; current_order <= MAX_ORDER; current_order++) {
    uint64_t coalesce_bits  = (g_buddy_blocks[current_order] + 1) >> 1;
    uint64_t coalesce_bytes = bytes_for_bits(coalesce_bits);

    g_buddy_coalesce[current_order] = (uint64_t*)(meta_virt + off);
    off += coalesce_bytes;
  }

  g_frame_refcount = (uint32_t*)(meta_virt + off);
  off += g_total_frames * sizeof(uint32_t);

  g_frame_pincount = (uint16_t*)(meta_virt + off);
  off += g_total_frames * sizeof(uint16_t);

  g_slab_trackers = (slab_cache_t**)(meta_virt + off);
  off += g_total_frames * sizeof(slab_cache_t*);

  g_slab_heads = (void**)(meta_virt + off);
  off += g_total_frames * sizeof(void*);

  zero(meta_virt, align_up(meta_size_bytes));

  // Initialize free lists
  for (uint8_t i = 0; i <= MAX_ORDER; i++) {
    g_free_lists[i] = NULL;
  }

  // Populate free lists using pmm_buddy_free
  // This automatically sets the XOR bits and handles merging
  for (uint32_t i = 0; i < count; i++) {
    if (map[i].type != 1) continue;

    uint64_t start = align_up(map[i].base);
    uint64_t end   = align_down(map[i].base + map[i].length);

    if (start >= (g_total_frames << PAGE_SHIFT)) continue;
    if (end   >  (g_total_frames << PAGE_SHIFT)) end = (g_total_frames << PAGE_SHIFT);
    if (start >= end) continue;

    for (uint64_t current = start; current < end; ) {
      uint64_t frame_index = (current >> PAGE_SHIFT);

      // Check if this frame overlaps reserved/kernel/metadata
      if (frame_index < PMM_RESERVED_FRAMES) {
        current += PAGE_SIZE;
        continue;
      }
      if (frame_index >= meta_frame_start_idx && frame_index < meta_frame_end_idx) {
        current += PAGE_SIZE;
        continue;
      }
      if (frame_index >= kernel_start_idx && frame_index < kernel_end_idx) {
        current += PAGE_SIZE;
        continue;
      }

      uint8_t current_order = calculate_max_order(current, end);

      // Reduce order until we fit without hitting forbidden regions
      while (1) {
        if (!block_overlaps_forbidden(frame_index,
                                      current_order,
                                      meta_frame_start_idx,
                                      meta_frame_end_idx,
                                      kernel_start_idx,
                                      kernel_end_idx)) {
          break;
        }
        if (current_order == 0) break;
        current_order--;
      }

      // Final check if order 0 is also forbidden
      if (block_overlaps_forbidden(frame_index,
                                   current_order,
                                   meta_frame_start_idx,
                                   meta_frame_end_idx,
                                   kernel_start_idx,
                                   kernel_end_idx)) {
        current += PAGE_SIZE;
        continue;
      }

      // Found a valid free block.  This runs during boot init on one
      // CPU before any other code can touch the allocator, so we call
      // the _locked variant directly (no lock needed).
      pmm_buddy_free_locked(current, current_order);

      current += order_to_bytes(current_order);
    }
  }

  pmm_count_free_pages();
}

// Internal implementation — caller holds g_pmm_lock.
static phys_addr_t pmm_buddy_alloc_locked(uint8_t order) {
  if (order > MAX_ORDER) return PMM_INVALID_ADDR;

  // 1. Find the smallest available block >= order
  uint8_t current_order = order;
  while (current_order <= MAX_ORDER) {
    if (g_free_lists[current_order]) break;
    current_order++;
  }

  if (current_order > MAX_ORDER) return PMM_INVALID_ADDR;

  // 2. Pop the block from the list
  phys_addr_t allocated_phys = free_list_pop(current_order);
  uint64_t frame_index = allocated_phys >> PAGE_SHIFT;

  uint64_t block_index = frame_index >> current_order;

  // 3. Toggle XOR bit to mark as allocated (state change)
  bit_toggle(g_buddy_coalesce[current_order], block_index >> 1);

  // 4. Split down to requested order
  while (current_order > order) {
    current_order--;

    uint64_t buddy_block_index = (block_index << 1) + 1;
    phys_addr_t buddy_phys = (phys_addr_t)(buddy_block_index << current_order) << PAGE_SHIFT;

    // Add the right child (buddy) to the free list
    free_list_push(current_order, buddy_phys);

    // Toggle XOR bit for the new split pair.
    // One is now allocated (the one we are holding), one is free (in list).
    // Initial state was 0 (both implied allocated inside the parent).
    // New state is Different -> 1.
    bit_toggle(g_buddy_coalesce[current_order], buddy_block_index >> 1);

    // We keep holding the left child
    block_index = (block_index << 1);
  }

  // NOTE: refcount is intentionally NOT touched here.  This function
  // is purely structural buddy bookkeeping.  The single place that
  // stamps an allocation's refcount is pmm_mark_allocated(), called
  // by the public allocators at hand-out — that keeps the
  // "free frame ⇒ rc==0, handed-out frame ⇒ rc==1" invariant in ONE
  // shared code path (refill, which parks frames in the pcp stash
  // WITHOUT handing them out, deliberately skips the mark).
  __atomic_fetch_sub(&g_free_pages, order_to_pages(order), __ATOMIC_RELAXED);
  return allocated_phys;
}

// ── Shared allocation-stamp chokepoint ───────────────────────────────────
// Every frame leaving the allocator to a real owner passes through here
// (pcp fast path + slow buddy path + slab grow).  Sets rc=1 and asserts
// the frame really was free (rc==0).  A nonzero prior count means the
// frame was freed while still referenced and then re-handed-out — the
// double-allocation class this centralisation exists to make impossible.
static void pmm_mark_allocated(phys_addr_t phys, uint8_t order) {
    uint64_t fi = phys >> PAGE_SHIFT;
    uint64_t n  = order_to_pages(order);
    for (uint64_t k = 0; k < n; k++) {
        if (fi + k >= g_total_frames) continue;
        uint32_t prev = __atomic_exchange_n(&g_frame_refcount[fi + k], 1,
                                             __ATOMIC_ACQ_REL);
        if (prev != 0) {
            extern void kprintf(const char* fmt, ...);
            kprintf("[pmm] DOUBLE-ALLOC frame=%p rc=%u order=%u\n",
                    (void*)(phys + k * 4096), (unsigned)prev, (unsigned)order);
        }
    }
}

// Internal implementation — caller holds g_pmm_lock.
static void pmm_buddy_free_locked(phys_addr_t addr, uint8_t order) {
  if (order > MAX_ORDER) return;

  uint64_t frame_index = (addr >> PAGE_SHIFT);
  if (frame_index >= g_total_frames) return;

  // Clear refcount for the whole freed block.  Direct buddy_free
  // callers (page-table frames, error paths) never went through
  // pmm_ref_dec, so without this their stale rc=1 survives onto the
  // free list and the next alloc inherits it — breaking the
  // "free frame ⇒ rc==0" invariant that the double-alloc detector
  // and the pinned-frame guard both rely on.
  {
      uint64_t n = order_to_pages(order);
      for (uint64_t k = 0; k < n; k++)
          if (frame_index + k < g_total_frames)
              g_frame_refcount[frame_index + k] = 0;
  }

  uint64_t block_index = (frame_index >> order);
  if (block_index >= g_buddy_blocks[order]) return;

  while (order < MAX_ORDER) {
    uint64_t pair_index = (block_index >> 1);

    if (pair_index >= ((g_buddy_blocks[order] + 1) >> 1)) break;

    // Toggle XOR bit.
    bit_toggle(g_buddy_coalesce[order], pair_index);

    // Check result:
    // If bit is 1: State is now Different. Buddy is allocated. We stop.
    if (bit_test(g_buddy_coalesce[order], pair_index)) {
      break;
    }

    // If bit is 0: State is now Same. Buddy must be Free.
    // Calculate buddy address and remove from list.
    uint64_t buddy_block_index = (block_index ^ 1ULL);
    phys_addr_t buddy_phys = (phys_addr_t)(buddy_block_index << order) << PAGE_SHIFT;

    free_list_remove(order, buddy_phys);

    // Promote
    block_index >>= 1;
    order++;
    
    // Ensure the new index is valid (address is naturally aligned to new order)
    addr &= ~(1ULL << (PAGE_SHIFT + order - 1)); 
  }

  // Add the final (merged) block to the free list
  free_list_push(order, addr);
}

// Called from pmm_init after all usable memory has been freed into the buddy.
static void pmm_count_free_pages(void) {
    uint64_t count = 0;
    for (uint8_t o = 0; o <= MAX_ORDER; o++) {
        free_block_t* b = g_free_lists[o];
        while (b) { count += order_to_pages(o); b = b->next; }
    }
    __atomic_store_n(&g_free_pages, count, __ATOMIC_RELEASE);
}

// ── Public buddy API — takes g_pmm_lock ───────────────────────────────
//
// Debug kill-switch: if PMM_DEBUG_ALWAYS_ZERO is 1, every freshly
// allocated frame is zeroed before it leaves the allocator.  This
// costs one 4 KiB memset per order-0 alloc (~500 cycles on modern
// x86), so it's a measurable slowdown on every fork, every brk
// growth, every demand-page, every CoW break.  But it turns every
// "uninitialised frame contents leak into user memory" bug into a
// benign clean read.
//
// Use during debugging: flip to 1, see if the PF-KILL / corruption
// symptom disappears.  If yes → some caller is handing out frames
// without zeroing → audit every pmm_buddy_alloc() caller.  If no →
// the corruption source is elsewhere.
//
// Independent of this switch, the specific paths that MUST zero
// (demand-page anon, CoW break destination, intermediate page
// tables, vmm_alloc_pml4) already do so.  This switch only changes
// whether UNREACHED paths are also covered.
// Debug kill-switch that zeros every buddy-allocated frame before
// returning it.  Useful for catching "uninitialised frame reuse
// leaks stale bytes into new code" classes of bugs.  Safe to leave
// at 1: any code path that reads stale bytes is broken regardless.
// Frame zeroing is ~500 cycles per 4 KiB page (memset rep stosq);
// dominated by the actual DRAM write bandwidth, not the loop.
#define PMM_DEBUG_ALWAYS_ZERO 0

// Phase 4: synchronous drain of every cache's empty-list.  Called
// from pmm_buddy_alloc() when the buddy is out of free blocks,
// before declaring OOM, and from the Phase 4F shrinker kthread.
// Caller holds g_pmm_lock.
void pmm_slab_shrink_all_locked(void) {
  for (slab_cache_t* c = g_slab_cache_head; c; c = c->cache_next) {
    while (c->empty) {
      slab_header_t* h = (slab_header_t*)c->empty;
      pmm_slab_destroy_locked(c, h);  // unlinks + frees to buddy
    }
  }
}

// Phase 5D: bounded shrink.  Walks each cache's empty list to find
// its TAIL (oldest — head-push means tail is LRU), then drains up
// to `max_per_cache` pages tail-first.  Caller holds g_pmm_lock.
//
// The per-call O(n) walk to tail is acceptable: shrinker runs at
// most every 10s under low pressure, empty lists are bounded by
// working set.
void pmm_slab_shrink_bounded_locked(uint32_t max_per_cache) {
  if (max_per_cache == 0) return;
  for (slab_cache_t* c = g_slab_cache_head; c; c = c->cache_next) {
    uint32_t remaining = max_per_cache;
    while (remaining && c->empty) {
      // Find tail of c->empty (doubly-linked via prev/next).
      slab_header_t* tail = (slab_header_t*)c->empty;
      while (tail->next) tail = (slab_header_t*)tail->next;
      pmm_slab_destroy_locked(c, tail);  // unlinks + frees to buddy
      remaining--;
    }
  }
}

uint32_t pmm_free_ratio_bp(void) {
  if (g_total_frames == 0) return 10000;
  uint64_t free = __atomic_load_n(&g_free_pages, __ATOMIC_RELAXED);
  if (free >= g_total_frames) return 10000;
  uint64_t bp = (free * 10000u) / g_total_frames;
  return (uint32_t)bp;
}

// ── Phase 4E hooks used by pcp.c ─────────────────────────────────────
// The PCP needs to take/release g_pmm_lock around batched refills and
// drains so it doesn't re-acquire once per page.  These wrappers
// expose the lock as a pair and the _locked buddy paths by name.
void pmm_pcp_lock(uint64_t* flags_out) {
    *flags_out = spin_lock_irqsave(&g_pmm_lock);
}
void pmm_pcp_unlock(uint64_t flags) {
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}
phys_addr_t pmm_buddy_alloc_locked_for_pcp(uint8_t order) {
    return pmm_buddy_alloc_locked(order);
}
void pmm_buddy_free_locked_for_pcp(phys_addr_t addr, uint8_t order) {
    pmm_buddy_free_locked(addr, order);
    __atomic_fetch_add(&g_free_pages, order_to_pages(order), __ATOMIC_RELAXED);
}

phys_addr_t pmm_buddy_alloc(uint8_t order) {
  // Phase 4E: order-0 goes through the per-CPU pcp fast path.  Only
  // order ≥ 1 takes g_pmm_lock on every call (rare in practice —
  // only VMM large mappings, DMA buffers, and the slab_order=1/2
  // slabs during refill).
  if (order == 0) {
    extern phys_addr_t pcp_alloc(void);
    phys_addr_t r = pcp_alloc();
    if (r != PMM_INVALID_ADDR) {
      pmm_mark_allocated(r, 0);          // shared stamp — see above
#if PMM_DEBUG_ALWAYS_ZERO
      __builtin_memset((void*)(r + HHDM_OFFSET), 0, PAGE_SIZE);
#endif
      return r;
    }
    // pcp refill failed → try direct buddy with shrink-on-pressure.
  }

  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  phys_addr_t r = pmm_buddy_alloc_locked(order);
  if (r == PMM_INVALID_ADDR) {
    // Hard pressure: reclaim every empty slab page we're sitting on
    // and retry once.  Guarantees we never fail an allocation while
    // holding reclaimable slab pages.
    pmm_slab_shrink_all_locked();
    r = pmm_buddy_alloc_locked(order);
  }
  if (r != PMM_INVALID_ADDR) pmm_mark_allocated(r, order);   // shared stamp
  spin_unlock_irqrestore(&g_pmm_lock, flags);
#if PMM_DEBUG_ALWAYS_ZERO
  if (r != PMM_INVALID_ADDR) {
    uint64_t bytes = (uint64_t)PAGE_SIZE << order;
    __builtin_memset((void*)(r + HHDM_OFFSET), 0, bytes);
  }
#endif
  return r;
}

void pmm_buddy_free(phys_addr_t addr, uint8_t order) {
  // Phase 4E: order-0 hits the per-CPU pcp push.
  if (order == 0) {
    extern void pcp_free(phys_addr_t);
    pcp_free(addr);
    return;
  }
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  pmm_buddy_free_locked(addr, order);
  __atomic_fetch_add(&g_free_pages, order_to_pages(order), __ATOMIC_RELAXED);
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

static inline uint64_t align_up_to(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

uint8_t calculate_slab_order(size_t slot_size) {
  uint8_t order;

  for (order = 0; order <= SLAB_MAX_ORDER; order++) {
    size_t slab_size = (size_t)PAGE_SIZE << order;
    if (slab_size < slot_size) continue;

    size_t objects = slab_size / slot_size;
    size_t waste = slab_size % slot_size;

    if (objects >= SLAB_MIN_OBJECTS && waste <= (slab_size / SLAB_WASTE_FRACTION))
      return order;
  }

  for (order = 0; order <= SLAB_MAX_ORDER; order++) {
    if (((size_t)PAGE_SIZE << order) >= slot_size)
      return order;
  }

  return INVALID_ORDER;
}

static inline void slab_list_remove(slab_header_t** head, slab_header_t* h) {
  if (h->prev) h->prev->next = h->next;
  else *head = h->next;

  if (h->next) h->next->prev = h->prev;

  h->prev = NULL;
  h->next = NULL;
}

static inline void slab_list_push(slab_header_t** head, slab_header_t* h) {
  h->prev = NULL;
  h->next = *head;
  if (*head) (*head)->prev = h;
  *head = h;
}

// Internal — caller holds g_pmm_lock.
static slab_header_t* pmm_slab_grow_locked(slab_cache_t* cache) {
  phys_addr_t slab_phys = pmm_buddy_alloc_locked(cache->slab_order);
  if (slab_phys == PMM_INVALID_ADDR) return NULL;
  pmm_mark_allocated(slab_phys, cache->slab_order);   // shared stamp

  uint64_t head_frame_index = (slab_phys >> PAGE_SHIFT);
  uint64_t pages_in_slab = (1ULL << cache->slab_order);

  // header lives at the start of the first page of the slab
  slab_header_t* h = (slab_header_t*)phys_to_virt(slab_phys);
  zero((virt_addr_t)h, sizeof(slab_header_t));

  h->cache = cache;
  h->slab_phys = slab_phys;
  h->freelist = NULL;
  h->inuse = 0;
  h->on_list = SLAB_LIST_NONE;
  h->remote_free = NULL;

  // update trackers and g_slab_heads for every page in the slab
  for (uint64_t page_offset = 0; page_offset < pages_in_slab; page_offset++) {
    uint64_t frame_index = head_frame_index + page_offset;
    if (frame_index >= g_total_frames) break;

    g_slab_trackers[frame_index] = cache;
    g_slab_heads[frame_index] = (void*)head_frame_index;
  }

  uint8_t* base = (uint8_t*)phys_to_virt(slab_phys);

  size_t slab_bytes = (size_t)PAGE_SIZE << cache->slab_order;

  // first slot starts after header, aligned to slot_size
  uint64_t first_slot_off = align_up_to((uint64_t)sizeof(slab_header_t), (uint64_t)cache->slot_size);
  if (first_slot_off >= slab_bytes) {
    // cannot fit any slot, return slab to buddy
    for (uint64_t page_offset = 0; page_offset < pages_in_slab; page_offset++) {
      uint64_t frame_index = head_frame_index + page_offset;
      if (frame_index >= g_total_frames) break;
      g_slab_trackers[frame_index] = NULL;
      g_slab_heads[frame_index] = NULL;
    }
    pmm_buddy_free_locked(slab_phys, cache->slab_order);
    return NULL;
  }

  size_t usable_bytes = slab_bytes - (size_t)first_slot_off;
  size_t num_slots = usable_bytes / cache->slot_size;

  void* freelist = NULL;
  for (size_t i = num_slots; i-- > 0; ) {
    void* slot = (void*)(base + first_slot_off + i * cache->slot_size);
    *(void**)slot = freelist;
    freelist = slot;
  }

  h->freelist = freelist;

  slab_list_push((slab_header_t**)&cache->partial, h);
  h->on_list = SLAB_LIST_PARTIAL;
  return h;
}

// Phase 4: pop a fully-empty page from cache->empty for reuse before
// hitting the buddy allocator.  Returns the page repurposed onto
// cache->partial (freelist already populated by an earlier
// initialisation), or NULL if empty list is dry.  Caller holds
// g_pmm_lock.
static slab_header_t* pmm_slab_recycle_empty_locked(slab_cache_t* cache) {
    slab_header_t* h = (slab_header_t*)cache->empty;
    if (!h) return NULL;
    slab_list_remove((slab_header_t**)&cache->empty, h);
    slab_list_push((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_PARTIAL;
    return h;
}

// Phase 5A: drain the per-page remote_free Treiber stack into
// h->freelist and decrement h->inuse by the count of drained items.
// Called at the head of every slow-path function that touches
// h->freelist or h->inuse so that cross-CPU frees (which push to
// remote_free lockless from slab_pcpu_free) are reflected before any
// state check.
//
// Caller must hold g_pmm_lock.  The atomic_exchange grabs the
// entire chain in one step; remote freers continue pushing to a
// fresh (post-exchange) NULL head without blocking.
//
// Chain length is typically small (few items between slow-path
// transitions) so the O(n) tail walk isn't a concern.
static inline void drain_remote_into_freelist_locked(slab_header_t* h) {
    void* chain = __atomic_exchange_n(&h->remote_free, NULL, __ATOMIC_ACQUIRE);
    if (!chain) return;
    // Walk to tail; count items.
    void*    tail = chain;
    uint32_t n    = 1;
    while (*(void**)tail) {
        tail = *(void**)tail;
        n++;
    }
    *(void**)tail = h->freelist;
    h->freelist   = chain;
    if (h->inuse >= n) h->inuse -= n;
    else               h->inuse = 0;  // defensive — should be unreachable
}

// Internal — caller holds g_pmm_lock.
static void* pmm_slab_alloc_locked(slab_cache_t* cache) {
  slab_header_t* h = (slab_header_t*)cache->partial;

  if (!h) {
    // Try recycling a fully-empty page before going to buddy.  Cheap
    // and avoids buddy churn when allocator pressure oscillates.
    h = pmm_slab_recycle_empty_locked(cache);
    if (!h) {
      h = pmm_slab_grow_locked(cache);
      if (!h) return NULL;
    }
  }

  // Phase 5A: absorb any cross-CPU frees that landed on this page
  // while it was waiting on cache->partial.  Keeps inuse and
  // freelist consistent before we touch them.
  drain_remote_into_freelist_locked(h);

  void* slot = h->freelist;
  if (!slot) {
    // After draining, still nothing.  Means remote_free was empty
    // and the page was on cache->partial with NULL freelist — that
    // should never happen (partial invariant: freelist != NULL)
    // unless a concurrent allocator already took the object.
    // Return NULL; caller will try again or report OOM.
    return NULL;
  }

  h->freelist = *(void**)slot;
  h->inuse++;

  if (!h->freelist) {
    slab_list_remove((slab_header_t**)&cache->partial, h);
    slab_list_push((slab_header_t**)&cache->full, h);
    h->on_list = SLAB_LIST_FULL;
  }

  return slot;
}

// Public wrapper — takes g_pmm_lock.
void* pmm_slab_alloc(slab_cache_t* cache) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  void* r = pmm_slab_alloc_locked(cache);
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return r;
}

// Internal — caller holds g_pmm_lock.
// Unlinks `h` from whichever cache list it lives on, clears the slab
// trackers for every frame in the slab, and returns the underlying
// pages to the buddy allocator.  After this call `h` is invalid.
// Helper: actually return a slab page to the buddy allocator.
// Clears per-frame trackers for every frame in the slab, frees the
// physical pages, and accounts for g_free_pages.  Caller MUST hold
// g_pmm_lock.  Shared between the immediate destroy path and the
// RCU-deferred destroy callback (Phase 5B).
static void pmm_slab_release_to_buddy_locked(slab_cache_t* cache, slab_header_t* h) {
  uint64_t head_frame_index = (h->slab_phys >> PAGE_SHIFT);
  uint64_t pages_in_slab = (1ULL << cache->slab_order);

  for (uint64_t page_offset = 0; page_offset < pages_in_slab; page_offset++) {
    uint64_t frame_index = head_frame_index + page_offset;
    if (frame_index >= g_total_frames) break;

    g_slab_trackers[frame_index] = NULL;
    g_slab_heads[frame_index] = NULL;
  }

  pmm_buddy_free_locked(h->slab_phys, cache->slab_order);
  __atomic_fetch_add(&g_free_pages, order_to_pages(cache->slab_order), __ATOMIC_RELAXED);
}

// Phase 5B: RCU callback — runs after a grace period has elapsed so
// every CPU that might have been dereferencing a pointer into this
// slab page has reached a quiescent state.  Clears trackers (making
// pmm_is_slab_ptr return false for these frames) and returns the
// pages to buddy.
//
// Runs in kthread context (rcu_gp_kthread), not IRQ — safe to take
// g_pmm_lock with irqsave.  The `cache` pointer is recovered from
// h->cache; the h itself is a valid slab_header_t because the page's
// underlying memory hasn't been freed yet (that's the whole point).
static void pmm_slab_rcu_free_cb(void* data) {
  slab_header_t* h     = (slab_header_t*)data;
  slab_cache_t*  cache = h->cache;
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  pmm_slab_release_to_buddy_locked(cache, h);
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

static void pmm_slab_destroy_locked(slab_cache_t* cache, slab_header_t* h) {
  // Phase 5A: drain any pending cross-CPU frees before returning the
  // page to buddy.  Items left on remote_free would be lost — the
  // memory's about to be reused for something else.
  drain_remote_into_freelist_locked(h);

  switch (h->on_list) {
    case SLAB_LIST_PARTIAL: slab_list_remove((slab_header_t**)&cache->partial, h); break;
    case SLAB_LIST_FULL:    slab_list_remove((slab_header_t**)&cache->full,    h); break;
    case SLAB_LIST_EMPTY:   slab_list_remove((slab_header_t**)&cache->empty,   h); break;
    default: break;  // SLAB_LIST_NONE: not on a list (caller already unlinked)
  }

  // Phase 5B: for typesafe caches, defer the actual buddy return via
  // call_rcu.  The page memory stays slab-typed (g_slab_trackers
  // still points at `cache`) for the duration of the grace period,
  // so any rcu_read_lock'd reader still dereferencing a pointer
  // into this page sees a valid object of the same type.  A new
  // alloc in the same cache may return a pointer into a different
  // (not-yet-reclaimed) page — the typesafe guarantee is per-type,
  // not per-instance, so instance identity is up to the user (seqlock
  // / version counter).
  if (cache->flags & SLAB_TYPESAFE_BY_RCU) {
    h->on_list = SLAB_LIST_RCU_DEFER;
    // call_rcu_head: zero-allocation, lock-free push.  Safe to call
    // while holding g_pmm_lock — the head is embedded in h, no
    // recursion into the allocator, no synchronize_rcu wait.  The
    // rcu_gp_kthread fires pmm_slab_rcu_free_cb after a grace period.
    call_rcu_head((rcu_head_t*)&h->rcu_head, pmm_slab_rcu_free_cb, h);
    return;
  }

  h->on_list = SLAB_LIST_NONE;
  pmm_slab_release_to_buddy_locked(cache, h);
}

// Internal — caller holds g_pmm_lock.
//
// Phase 4 page state machine on free:
//   - was on FULL  → move to PARTIAL
//   - was on PARTIAL or EMPTY → stay (PARTIAL after this push has a
//     valid freelist again, so it remains on PARTIAL — EMPTY shouldn't
//     happen here because EMPTY pages have inuse==0 and no live ptrs).
//   - inuse drops to 0 → move to EMPTY (the shrinker (Phase 4F) will
//     reclaim it; until then it's a recycle candidate via
//     pmm_slab_recycle_empty_locked, avoiding buddy churn).
static void pmm_slab_free_locked(void* ptr) {
  if (!ptr) return;

  phys_addr_t phys = virt_to_phys((virt_addr_t)ptr);
  uint64_t frame_index = (phys >> PAGE_SHIFT);
  if (frame_index >= g_total_frames) return;

  slab_cache_t* cache = g_slab_trackers[frame_index];
  if (!cache) return;

  uint64_t head_idx = (uint64_t)g_slab_heads[frame_index];
  slab_header_t* h = (slab_header_t*)phys_to_virt((phys_addr_t)(head_idx << PAGE_SHIFT));

  // Phase 5A: absorb cross-CPU frees into freelist/inuse BEFORE we
  // inspect/update them, so the page state transitions below see a
  // consistent view.
  drain_remote_into_freelist_locked(h);

  uint8_t was_on = h->on_list;

  *(void**)ptr = h->freelist;
  h->freelist = ptr;

  if (h->inuse) h->inuse--;

  // FULL → PARTIAL transition (we just gave the page a free slot).
  if (was_on == SLAB_LIST_FULL) {
    slab_list_remove((slab_header_t**)&cache->full, h);
    slab_list_push((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_PARTIAL;
  }

  // inuse hit 0 → move from PARTIAL to EMPTY for shrinker reclaim.
  // The page sits on cache->empty until either the shrinker drains
  // it or pmm_slab_recycle_empty_locked promotes it back to PARTIAL.
  if (h->inuse == 0 && h->on_list == SLAB_LIST_PARTIAL) {
    slab_list_remove((slab_header_t**)&cache->partial, h);
    slab_list_push((slab_header_t**)&cache->empty, h);
    h->on_list = SLAB_LIST_EMPTY;
  }
}

// Public wrapper — takes g_pmm_lock.
void pmm_slab_free(void* ptr) {
  if (!ptr) return;
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  pmm_slab_free_locked(ptr);
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint8_t pmm_is_slab_ptr(void* ptr) {
  if (!ptr) return 0;
  phys_addr_t phys = virt_to_phys((virt_addr_t)ptr);
  uint64_t frame_index = (phys >> PAGE_SHIFT);
  if (frame_index >= g_total_frames) return 0;
  return g_slab_trackers[frame_index] != NULL;
}

phys_addr_t pmm_highest_address_get(void) {
  return g_phys_ceiling;
}

void pmm_slab_cache_init(slab_cache_t* cache, size_t slot_size, uint8_t flags) {
  cache->slot_size = slot_size;
  cache->partial = NULL;
  cache->full = NULL;
  cache->empty = NULL;
  cache->slab_order = calculate_slab_order(slot_size);
  cache->class_idx = (uint8_t)0xFF;   // kheap sets this for managed caches
  cache->flags = flags;

  // Pre-compute objects per slab page for slab_pcpu promote/demote.
  // Mirrors the layout pmm_slab_grow_locked uses: header at offset 0,
  // first slot at align_up(sizeof(header), slot_size), slots packed.
  size_t   slab_bytes     = (size_t)PAGE_SIZE << cache->slab_order;
  uint64_t first_slot_off = align_up_to((uint64_t)sizeof(slab_header_t),
                                         (uint64_t)slot_size);
  if (first_slot_off >= slab_bytes) {
      cache->obj_per_slab = 0;
  } else {
      size_t num_slots = (slab_bytes - first_slot_off) / slot_size;
      cache->obj_per_slab = (num_slots > 0xFFFF) ? 0xFFFF : (uint16_t)num_slots;
  }

  // Phase 5C: adaptive CPU partial list cap.  Linux SLUB's heuristic
  // (mm/slub.c get_order_size_cpu_partial): scale inversely with
  // obj_per_slab so small-object caches accumulate more partial
  // pages per CPU.  Capped at SLAB_PCPU_PARTIAL_DEPTH (compile-time
  // upper bound of the on-CPU list).
  uint16_t cap;
  if      (cache->obj_per_slab >= 256) cap = 13;
  else if (cache->obj_per_slab >= 32)  cap = 6;
  else                                 cap = 2;
  if (cap > SLAB_PCPU_PARTIAL_DEPTH) cap = SLAB_PCPU_PARTIAL_DEPTH;
  cache->cpu_partial_cap = cap;

  // Register on the global cache list so the Phase 4F shrinker and
  // Phase 4H stats walker can find every cache.  Boot-time only;
  // no synchronisation needed (single-threaded init).
  cache->cache_next = g_slab_cache_head;
  g_slab_cache_head = cache;
}

// ── Phase 4 grab/park helpers for slab_pcpu.c ─────────────────────────

slab_header_t* pmm_slab_grab_partial(slab_cache_t* cache) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_header_t* h = (slab_header_t*)cache->partial;
  if (h) {
    slab_list_remove((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_NONE;
    // Phase 5A: absorb cross-CPU frees that landed while on the
    // partial list.  Caller gets a fully-merged page.
    drain_remote_into_freelist_locked(h);
  }
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return h;
}

slab_header_t* pmm_slab_grab_empty(slab_cache_t* cache) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_header_t* h = (slab_header_t*)cache->empty;
  if (h) {
    slab_list_remove((slab_header_t**)&cache->empty, h);
    h->on_list = SLAB_LIST_NONE;
    drain_remote_into_freelist_locked(h);
  }
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return h;
}

slab_header_t* pmm_slab_grow(slab_cache_t* cache) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_header_t* h = pmm_slab_grow_locked(cache);
  if (h) {
    // grow puts it on partial; unlink so the caller takes ownership.
    slab_list_remove((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_NONE;
    // Fresh page, remote_free is NULL from grow_locked's init — but
    // drain anyway in case future code paths share this helper from
    // a non-init state.
    drain_remote_into_freelist_locked(h);
  }
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return h;
}

void pmm_slab_park_partial(slab_cache_t* cache, slab_header_t* h) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_list_push((slab_header_t**)&cache->partial, h);
  h->on_list = SLAB_LIST_PARTIAL;
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_slab_park_full(slab_cache_t* cache, slab_header_t* h) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_list_push((slab_header_t**)&cache->full, h);
  h->on_list = SLAB_LIST_FULL;
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_slab_park_empty(slab_cache_t* cache, slab_header_t* h) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  slab_list_push((slab_header_t**)&cache->empty, h);
  h->on_list = SLAB_LIST_EMPTY;
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

// ── Phase 5C: batched grab/park for per-CPU partial list ───────────────
// Both helpers do ONE g_pmm_lock acquire for up to `want`/`count`
// pages.  Amortises lock cost across many pages under bursty
// allocation (fork storms, mass-close, bulk network RX).

uint32_t pmm_slab_grab_partial_batch(slab_cache_t* cache,
                                      slab_header_t** out, uint32_t want) {
  if (want == 0) return 0;
  uint32_t got = 0;
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  while (got < want) {
    slab_header_t* h = (slab_header_t*)cache->partial;
    if (!h) break;
    slab_list_remove((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_NONE;
    // Drain remote frees into h->freelist while we still hold the
    // lock — caller gets a consistent page.
    drain_remote_into_freelist_locked(h);
    out[got++] = h;
  }
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return got;
}

void pmm_slab_park_partial_batch(slab_cache_t* cache,
                                  slab_header_t** pages, uint32_t count) {
  if (count == 0) return;
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  for (uint32_t i = 0; i < count; i++) {
    slab_header_t* h = pages[i];
    slab_list_push((slab_header_t**)&cache->partial, h);
    h->on_list = SLAB_LIST_PARTIAL;
  }
  spin_unlock_irqrestore(&g_pmm_lock, flags);
}

// Phase 4G: demote a CPU_ACTIVE page, or keep it active if cross-CPU
// frees populated its freelist.  Takes g_pmm_lock so the check of
// h->freelist is consistent with any concurrent pmm_slab_free_locked
// pushes.  See pmm.h for semantics.
int pmm_slab_demote_or_keep(slab_cache_t* cache, slab_header_t* h, void** out_fl) {
  uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
  // Phase 5A: drain cross-CPU frees into h->freelist before the
  // rescue check.  Any lockless push to h->remote_free that landed
  // while we owned the page is now materialised; the "kept active"
  // fast-exit republishes the merged chain via cpu_slot.
  drain_remote_into_freelist_locked(h);
  void* fl = h->freelist;
  if (fl) {
    // Cross-CPU frees populated the page's freelist while we were
    // CPU_ACTIVE.  Transfer ownership back to the caller's cpu_slot;
    // keep the page CPU_ACTIVE.
    h->freelist = NULL;
    *out_fl = fl;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return 1;
  }
  *out_fl = NULL;
  // No frees to rescue — park as FULL.  Page has no free slots from
  // anyone's view; future frees (via pmm_slab_free_locked) will
  // transition FULL→PARTIAL naturally.
  slab_list_push((slab_header_t**)&cache->full, h);
  h->on_list = SLAB_LIST_FULL;
  spin_unlock_irqrestore(&g_pmm_lock, flags);
  return 0;
}

slab_cache_t* pmm_slab_cache_first(void) {
  return g_slab_cache_head;
}

// ── Phase 4 accessors used by the per-CPU slab fast path ──────────────
//
// pmm_slab_state_get: returns the SLAB_LIST_* enum for the page that
// backs `phys`, or SLAB_LIST_NONE if the frame is not a slab page.
// The state is read directly from the slab_header_t — no separate
// state byte array — because the header already lives in the slab
// page itself (cache-hot if you've just allocated/freed an object on
// that page).
//
// pmm_slab_header_of: returns the slab_header_t* for the slab page
// that contains `ptr`.  NULL if `ptr` is not in a slab page.
//
// pmm_slab_cache_of: returns the slab_cache_t* that owns `ptr`'s
// slab page.  NULL if `ptr` is not in a slab page.

slab_header_t* pmm_slab_header_of(void* ptr) {
  if (!ptr) return NULL;
  phys_addr_t phys = virt_to_phys((virt_addr_t)ptr);
  uint64_t fi = (phys >> PAGE_SHIFT);
  if (fi >= g_total_frames) return NULL;
  if (!g_slab_trackers[fi]) return NULL;
  uint64_t head_idx = (uint64_t)g_slab_heads[fi];
  return (slab_header_t*)phys_to_virt((phys_addr_t)(head_idx << PAGE_SHIFT));
}

slab_cache_t* pmm_slab_cache_of(void* ptr) {
  if (!ptr) return NULL;
  phys_addr_t phys = virt_to_phys((virt_addr_t)ptr);
  uint64_t fi = (phys >> PAGE_SHIFT);
  if (fi >= g_total_frames) return NULL;
  return g_slab_trackers[fi];
}

uint8_t pmm_slab_state_get(phys_addr_t phys) {
  uint64_t fi = (phys >> PAGE_SHIFT);
  if (fi >= g_total_frames) return SLAB_LIST_NONE;
  if (!g_slab_trackers[fi]) return SLAB_LIST_NONE;
  uint64_t head_idx = (uint64_t)g_slab_heads[fi];
  slab_header_t* h = (slab_header_t*)phys_to_virt((phys_addr_t)(head_idx << PAGE_SHIFT));
  return h->on_list;
}

uint64_t pmm_total_frames_get(void) { return g_total_frames; }
uint64_t pmm_free_pages_get(void) {
    return __atomic_load_n(&g_free_pages, __ATOMIC_RELAXED);
}

// ── Per-frame refcount API ───────────────────────────────────────────────

void pmm_ref_inc(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    // TRIPWIRE: incrementing a frame with rc==0 resurrects a page that
    // is currently FREE (on the buddy/pcp free list).  That is a
    // use-after-free — the caller is bumping a frame the allocator
    // already gave to (or is about to give to) someone else.  Catch it
    // at the inc, where the caller's return address points straight at
    // the offending site.
    if (g_frame_refcount[fi] == 0) {
        extern void kprintf(const char* fmt, ...);
        kprintf("[pmm] REF-INC-ON-FREE frame=%p caller=%p\n",
                (void*)addr, __builtin_return_address(0));
    }
    g_frame_refcount[fi]++;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_ref_dec(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (g_frame_refcount[fi] == 0) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return;  // safety: already freed
    }
    g_frame_refcount[fi]--;
    int last_ref = (g_frame_refcount[fi] == 0);
    if (last_ref) {
        // NEVER free a pinned frame — DMA may be in flight (AHCI
        // scatter-gather pins its target pages).  A racing double-dec
        // dropping a pinned frame to the buddy let the next allocator
        // reuse it while the HBA was still writing file data into it.
        // Leak it instead and scream; the pin owner's unpin can't
        // resurrect the ref, so a leak here always indicates a bug
        // upstream — but a leak is debuggable, silent reuse is not.
        if (g_frame_pincount[fi] > 0) {
            extern void kprintf(const char* fmt, ...);
            kprintf("[pmm] BUG: last ref dropped on PINNED frame %p "
                    "(pins=%u) — leaking\n", (void*)addr,
                    (unsigned)g_frame_pincount[fi]);
        } else {
            pmm_buddy_free_locked(addr, 0);
            __atomic_fetch_add(&g_free_pages, 1, __ATOMIC_RELAXED);
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_ref_zero(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    __atomic_store_n(&g_frame_refcount[fi], 0, __ATOMIC_RELEASE);
}

uint32_t pmm_ref_get(phys_addr_t addr) {
    // Read-only access.  The value may race with a concurrent
    // inc/dec but each individual read is intact (uint32_t aligned
    // reads are atomic on x86).  Callers that need a stable count
    // should hold the appropriate higher-level lock.
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return 0;
    return g_frame_refcount[fi];
}

// ── Per-frame pin count API ──────────────────────────────────────────────

void pmm_pin(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    g_frame_pincount[fi]++;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_unpin(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (g_frame_pincount[fi] > 0) g_frame_pincount[fi]--;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint16_t pmm_pin_get(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return 0;
    return g_frame_pincount[fi];
}
