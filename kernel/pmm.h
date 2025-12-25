#pragma once
#include "common.h"

#define MAX_RAM_BYTES (64ULL * 1024 * 1024 * 1024)
#define MAX_FRAMES (MAX_RAM_BYTES / PAGE_SIZE)
#define BITMAP_BYTES ((MAX_FRAMES + 7) / 8)
#define PMM_INVALID_FRAME UINT64_MAX
#define PMM_RESERVED_FRAMES ((64 * 1024 * 1024) >> PAGE_SHIFT)

static uint64_t g_frame_bitmap[BITMAP_BYTES];

static uint64_t g_frame_count;  // actual frames present from E820

void pmm_init_from_map(e820_entry_t* map, uint32_t count); 
phys_addr_t pmm_frame_alloc(void);
void pmm_frame_free(uint64_t frame_idx); 
phys_addr_t pmm_highest_address_get(void);
void pmm_init(e820_entry_t* map, uint32_t count);
 
