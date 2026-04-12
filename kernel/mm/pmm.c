#include "pmm.h"
#include "common.h"

static phys_addr_t g_phys_ceiling = 0;
static uint64_t    g_total_frames = 0;

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

      // Found a valid free block. Insert it.
      pmm_buddy_free(current, current_order);

      current += order_to_bytes(current_order);
    }
  }
}

phys_addr_t pmm_buddy_alloc(uint8_t order) {
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

  // Set refcount = 1 for every frame in the allocated block.
  {
      uint64_t fi = allocated_phys >> PAGE_SHIFT;
      uint64_t n  = order_to_pages(order);
      for (uint64_t k = 0; k < n; k++)
          if (fi + k < g_total_frames)
              g_frame_refcount[fi + k] = 1;
  }

  return allocated_phys;
}

void pmm_buddy_free(phys_addr_t addr, uint8_t order) {
  if (order > MAX_ORDER) return;

  uint64_t frame_index = (addr >> PAGE_SHIFT);
  if (frame_index >= g_total_frames) return;

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

static slab_header_t* pmm_slab_grow(slab_cache_t* cache) {
  phys_addr_t slab_phys = pmm_buddy_alloc(cache->slab_order);
  if (slab_phys == PMM_INVALID_ADDR) return NULL;

  uint64_t head_frame_index = (slab_phys >> PAGE_SHIFT);
  uint64_t pages_in_slab = (1ULL << cache->slab_order);

  // header lives at the start of the first page of the slab
  slab_header_t* h = (slab_header_t*)phys_to_virt(slab_phys);
  zero((virt_addr_t)h, sizeof(slab_header_t));

  h->cache = cache;
  h->slab_phys = slab_phys;
  h->freelist = NULL;
  h->inuse = 0;
  h->in_full_list = 0;

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
    pmm_buddy_free(slab_phys, cache->slab_order);
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
  return h;
}

void* pmm_slab_alloc(slab_cache_t* cache) {
  slab_header_t* h = (slab_header_t*)cache->partial;

  if (!h) {
    h = pmm_slab_grow(cache);
    if (!h) return NULL;
  }

  void* slot = h->freelist;
  if (!slot) return NULL;

  h->freelist = *(void**)slot;
  h->inuse++;

  if (!h->freelist) {
    slab_list_remove((slab_header_t**)&cache->partial, h);
    slab_list_push((slab_header_t**)&cache->full, h);
    h->in_full_list = 1;
  }

  return slot;
}

static void pmm_slab_destroy(slab_cache_t* cache, slab_header_t* h) {
  if (h->in_full_list) slab_list_remove((slab_header_t**)&cache->full, h);
  else slab_list_remove((slab_header_t**)&cache->partial, h);

  uint64_t head_frame_index = (h->slab_phys >> PAGE_SHIFT);
  uint64_t pages_in_slab = (1ULL << cache->slab_order);

  for (uint64_t page_offset = 0; page_offset < pages_in_slab; page_offset++) {
    uint64_t frame_index = head_frame_index + page_offset;
    if (frame_index >= g_total_frames) break;

    g_slab_trackers[frame_index] = NULL;
    g_slab_heads[frame_index] = NULL;
  }

  pmm_buddy_free(h->slab_phys, cache->slab_order);
}

void pmm_slab_free(void* ptr) {
  if (!ptr) return;

  phys_addr_t phys = virt_to_phys((virt_addr_t)ptr);
  uint64_t frame_index = (phys >> PAGE_SHIFT);
  if (frame_index >= g_total_frames) return;

  slab_cache_t* cache = g_slab_trackers[frame_index];
  if (!cache) return;

  uint64_t head_idx = (uint64_t)g_slab_heads[frame_index];
  slab_header_t* h = (slab_header_t*)phys_to_virt((phys_addr_t)(head_idx << PAGE_SHIFT));

  int was_full = (h->freelist == NULL);

  *(void**)ptr = h->freelist;
  h->freelist = ptr;

  if (h->inuse) h->inuse--;

  if (was_full) {
    slab_list_remove((slab_header_t**)&cache->full, h);
    slab_list_push((slab_header_t**)&cache->partial, h);
    h->in_full_list = 0;
  }

  if (h->inuse == 0) {
    pmm_slab_destroy(cache, h);
  }
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

void pmm_slab_cache_init(slab_cache_t* cache, size_t slot_size) {
  cache->slot_size = slot_size;
  cache->partial = NULL;
  cache->full = NULL;
  cache->slab_order = calculate_slab_order(slot_size);
}

uint64_t pmm_total_frames_get(void) { return g_total_frames; }

// ── Per-frame refcount API ───────────────────────────────────────────────

void pmm_ref_inc(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi < g_total_frames) g_frame_refcount[fi]++;
}

void pmm_ref_dec(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return;
    if (g_frame_refcount[fi] == 0) return;  // safety: already freed
    g_frame_refcount[fi]--;
    if (g_frame_refcount[fi] == 0)
        pmm_buddy_free(addr, 0);
}

uint32_t pmm_ref_get(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return 0;
    return g_frame_refcount[fi];
}

// ── Per-frame pin count API ──────────────────────────────────────────────

void pmm_pin(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi < g_total_frames) g_frame_pincount[fi]++;
}

void pmm_unpin(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi < g_total_frames && g_frame_pincount[fi] > 0)
        g_frame_pincount[fi]--;
}

uint16_t pmm_pin_get(phys_addr_t addr) {
    uint64_t fi = addr >> PAGE_SHIFT;
    if (fi >= g_total_frames) return 0;
    return g_frame_pincount[fi];
}
