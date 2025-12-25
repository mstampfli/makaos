#include "pmm.h"
#include "common.h"

static phys_addr_t g_phys_ceiling = 0;

static inline void frame_bitmap_set(uint64_t frame_idx) {
    // frame_idx / 64
    g_frame_bitmap[frame_idx >> 6] |= (1ULL << (frame_idx & 63));
}

static inline void frame_bitmap_clear(uint64_t frame_idx) {
    // frame_idx / 64
    g_frame_bitmap[frame_idx >> 6] &= ~(1ULL << (frame_idx & 63));
}

static inline int frame_bitmap_test(uint64_t frame_idx) {
    // Check if bit is set
    return (g_frame_bitmap[frame_idx >> 6] >> (frame_idx & 63)) & 1ULL;
}
static inline uint64_t align_down(uint64_t addr) {
    return (addr & ~(PAGE_SIZE - 1));
}

static inline uint64_t align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/**
 * Parses E820 map to initialize the physical frame allocator.
 * Initially marks all frames as used (0xFF) to handle holes/reserved memory,
 * then clears bits for available RAM (Type 1).
 */
void pmm_init_from_map(e820_entry_t* map, uint32_t count) {
    // Pessimistic initialization: assume everything is reserved
    for (uint64_t i = 0; i < BITMAP_BYTES; i++) {
        g_frame_bitmap[i] = 0xFF;
    }

    for (uint32_t i = 0; i < count; i++) {
        e820_entry_t entry = map[i];
        
        uint64_t end = entry.base + entry.length;
        if (end > g_phys_ceiling) {
          g_phys_ceiling = end;
        }

        // Only process Usable RAM
        if (entry.type != 1) continue;

        uint64_t start = align_up(entry.base);
        end = align_down(entry.base + entry.length);
        // shrink range to ensure only fully contained pages are used
        
        uint64_t start_idx = start >> PAGE_SHIFT;
        uint64_t end_idx = end >> PAGE_SHIFT;

        // bounds checking against static MAX_FRAMES limit
        if (start_idx >= MAX_FRAMES) continue;
        if (end_idx > MAX_FRAMES) end_idx = MAX_FRAMES;
        if (start_idx >= end_idx) continue;

        // mark valid RAM as free
        for (uint64_t j = start_idx; j < end_idx; j++) {
            frame_bitmap_clear(j);
        }
    }

    for (uint64_t i = 0; i < PMM_RESERVED_FRAMES; i++) {
        frame_bitmap_set(i);
    }
}

phys_addr_t pmm_highest_address_get(void) {
  return g_phys_ceiling;
}

phys_addr_t pmm_frame_alloc(void) {
  for (uint64_t frame_block_idx = 0; frame_block_idx < MAX_FRAMES; frame_block_idx++) {
    uint64_t frame_block = g_frame_bitmap[frame_block_idx];

    if (frame_block == ~0ULL) continue;

    uint64_t bit_offset = __builtin_ctzll(~frame_block);
    uint64_t frame_idx = (frame_block_idx << 6) | bit_offset;

    frame_bitmap_set(frame_idx);
      
    return (phys_addr_t)(frame_idx << PAGE_SHIFT);
  }
  return PMM_INVALID_FRAME;
}

void pmm_frame_free(uint64_t frame_idx) {
  if (frame_idx < PMM_RESERVED_FRAMES) return;
  frame_bitmap_clear(frame_idx);
}

void pmm_init(e820_entry_t* map, uint32_t count) {
  pmm_init_from_map(map, count);

  uint64_t frame_start = align_down((uintptr_t)__kernel_start);
  uint64_t frame_end = align_up((uintptr_t)__kernel_end);
  uint64_t frame_start_idx = frame_start >> PAGE_SHIFT;
  uint64_t frame_end_idx = frame_end >> PAGE_SHIFT;

  for (uint64_t i = frame_start_idx; i < frame_end_idx; i++) frame_bitmap_set(i);
}

