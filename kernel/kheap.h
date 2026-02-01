#pragma once
#include "common.h"
#include "pmm.h"

typedef struct kheap {
  phys_addr_t base;
  phys_addr_t end;
  size_t cache_size_max;   // 1 byte per heap page (simple) or bitset later
  size_t num_chaches;

  slab_cache_t* caches;
} kheap_t;

void   kheap_init(void);
void*  kmalloc(size_t bytes);

#define BLOCK_FREE 1ULL
#define BLOCK_SIZE_MASK (~0x7ULL)  // clear lower 3 bits
#define KMALLOC_CACHE_COUNT 11
