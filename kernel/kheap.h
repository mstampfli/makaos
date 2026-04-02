#pragma once
#include "common.h"
#include "pmm.h"

#define KMALLOC_CACHE_COUNT 11
#define BLOCK_FREE          1ULL
#define BLOCK_SIZE_MASK     (~0x7ULL)

typedef struct kheap {
  virt_addr_t base;          // first virtual address available to the heap
  virt_addr_t end;           // top of virtual address space for heap
  size_t      cache_size_max;
  slab_cache_t caches[KMALLOC_CACHE_COUNT];
} kheap_t;

void   kheap_init(void);
void*  kmalloc(size_t bytes);
void   kfree(void* ptr);
