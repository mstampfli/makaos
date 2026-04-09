#include "kheap.h"
static kheap_t g_kheap;

static const size_t g_kmalloc_sizes[KMALLOC_CACHE_COUNT] = {
    8, 16, 32, 64,
    128, 256, 512,
    1024, 2048, 4096,
    // 8192 and above go directly to buddy allocator (slab needs 64KB blocks
    // to satisfy SLAB_MIN_OBJECTS=8, which is wasteful and often fails).
};

static inline uint64_t align_down(virt_addr_t addr) {
    return (addr & ~(PAGE_SIZE - 1));
}

static inline uint64_t align_up(virt_addr_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline bool is_heap_address(virt_addr_t addr) {
  return !(addr < g_kheap.base || addr > g_kheap.end);
}

static inline size_t pick_cache_idx(size_t size) {
  for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
    if (size <= g_kmalloc_sizes[i]) return i;
  }      
    
  return UINT64_MAX; 
}

static inline uint8_t size_to_order(size_t size) {
    if (size <= PAGE_SIZE) return 0;
    return (uint8_t)(64 - __builtin_clzll(size - 1)) - PAGE_SHIFT;
}

void kheap_init(void) {
  // heap occupies all virtual space after the kernel image
  g_kheap.base = align_up((virt_addr_t)__kernel_end);
  g_kheap.end  = 0xFFFFFFFFFFFFFFFFULL;
  g_kheap.cache_size_max = 2 * PAGE_SIZE;

  for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
    pmm_slab_cache_init(&g_kheap.caches[i], g_kmalloc_sizes[i]);
  }
}

void* kmalloc(size_t size) {
  size_t idx = pick_cache_idx(size);
  
  if (idx != UINT64_MAX) {
    return pmm_slab_alloc(&g_kheap.caches[idx]);
  }

  uint8_t order = size_to_order(size + 8);
  phys_addr_t phys = pmm_buddy_alloc(order);
  if (phys == PMM_INVALID_ADDR) return NULL;

  uint8_t* raw = (uint8_t*)(phys + HHDM_OFFSET);
  *raw = order;
  return (void*)(raw + 16);  // 16-byte aligned (raw is page-aligned)
}

void kfree(void* addr) {
  if (!addr) return;

  if (pmm_is_slab_ptr(addr)) {
    pmm_slab_free(addr);
  } else {
    uint8_t* raw = (uint8_t*)addr - 16;
    uint8_t order = *raw;
    phys_addr_t phys = (phys_addr_t)((virt_addr_t)raw - HHDM_OFFSET);
    pmm_buddy_free(phys, order);
  }
}

//LEGACY
/*
void kheap_init(void) {
  g_kheap.base = align_up((virt_addr_t)__kernel_end) + 4 * 1024;
  g_kheap.end = g_kheap.base + 128ULL * 1024 * 1024;

  uint64_t* metadata = (uint64_t*)g_kheap.base; 
  uint64_t size = (g_kheap.end - g_kheap.base) & BLOCK_SIZE_MASK;
  metadata[0] = size | BLOCK_FREE;
}

void* kmalloc(size_t bytes) {
  bytes = (bytes + 7) & ~0x7;

  uint64_t heap_size = (g_kheap.end - g_kheap.base) & BLOCK_SIZE_MASK;
  uint64_t highest_idx = (heap_size >> 3) - 1; 
  uint64_t* heap = (uint64_t*)g_kheap.base;
  
  uint64_t i = 0;
  bool found = false;
  bool check_next = false;
  uint64_t full_block_start_idx = 0;
  uint64_t found_idx = 0;
  uint64_t contiguous_block_size = 0;

  while (!found) {
    if (i > highest_idx) break;
    
    bool free = heap[i] & BLOCK_FREE;
    uint64_t size = heap[i] & BLOCK_SIZE_MASK;
    
    if (!free) {
      i += size / 8;
      contiguous_block_size = 0;
      check_next = false;
      continue;
    }
   
    if (size < (bytes + 8)) {
      if (check_next) {
        contiguous_block_size = ((i - full_block_start_idx) * 8) + size;
        if (contiguous_block_size >= (bytes + 8)) {
          found_idx = full_block_start_idx;
          found = true;
          break;
        }
      } else {
        full_block_start_idx = i; 
        check_next = true;
      }

      i += size / 8;
      continue;
    } 
    
    full_block_start_idx = i;
    contiguous_block_size = size;
    found_idx = i;
    found = true;
  }
  
  if (!found) return NULL;
  
  uint64_t new_size_after = contiguous_block_size - (bytes + 8);
  uint64_t new_block_idx = full_block_start_idx + ((bytes + 8) / 8);
  heap[new_block_idx] = (new_size_after & BLOCK_SIZE_MASK) | BLOCK_FREE;
  
  heap[full_block_start_idx] = (bytes + 8) & BLOCK_SIZE_MASK | ~BLOCK_FREE; 
  return (void*)(g_kheap.base + full_block_start_idx * 8 + 8);
}

void kfree(void* addr) {
  if ((virt_addr_t)addr & 0x7) return;
  
  uint64_t heap_size = (g_kheap.end - g_kheap.base) & BLOCK_SIZE_MASK;
  uint64_t highest_idx = (heap_size >> 3) - 1; 

  uint64_t heap_idx = ((virt_addr_t)addr - g_kheap.base) / 8;
  if (heap_idx > highest_idx) return;

  uint64_t* heap = (uint64_t*)g_kheap.base;
  
  heap[heap_idx - 1] |= BLOCK_FREE; 
}*/


