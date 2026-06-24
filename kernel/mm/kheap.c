#include "kheap.h"
#include "slab_pcpu.h"
#ifdef MAKAOS_BOOT_SELFTESTS
#include "kprintf.h"
#endif
static kheap_t g_kheap;

// Exposed to slab_pcpu.c — the only consumer that needs the cache
// pointer for a kmalloc class index.  Bounds-checked.
slab_cache_t* kheap_cache_get(uint8_t cls) {
    if (cls >= KMALLOC_CACHE_COUNT) return NULL;
    return &g_kheap.caches[cls];
}

static const size_t g_kmalloc_sizes[KMALLOC_CACHE_COUNT] = {
    8, 16, 32, 64,
    128, 256, 512,
    1024, 2048, 4096,
    // 8192 and above go directly to buddy allocator (slab needs 64KB blocks
    // to satisfy SLAB_MIN_OBJECTS=8, which is wasteful and often fails).
};

static inline uint64_t align_up(virt_addr_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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
    // Generic kmalloc caches: no RCU typesafe semantics, default flags.
    pmm_slab_cache_init(&g_kheap.caches[i], g_kmalloc_sizes[i], 0);
  }

  // Phase 4: stamp class_idx into every cache so slab_pcpu_free can
  // derive cls in O(1), and seed any further per-class state.
  // Per-CPU slots themselves are BSS-zeroed and self-seed on first
  // alloc via the slow refill path.
  slab_pcpu_init();
}

// Bytes reserved before the returned pointer for big (buddy-backed)
// allocations: the order byte lives at raw[0] and the payload starts at
// raw + KMALLOC_BIG_HDR (16-byte aligned).  This MUST be the single source of
// truth for both the size rounding and the pointer math — they previously
// disagreed (sized for 8, offset by 16), so a request in the band
// (2^k - 16, 2^k - 8] over-ran the buddy block by up to 8 bytes.
#define KMALLOC_BIG_HDR 16

void* kmalloc(size_t size) {
  size_t idx = pick_cache_idx(size);

  if (idx != UINT64_MAX) {
    // Phase 4 fast path: lockless cmpxchg16b pop on this CPU's
    // cpu_slot[idx].  Falls through to slab_refill on cold cache.
    return slab_pcpu_alloc(idx);
  }

  if (size > (size_t)-1 - KMALLOC_BIG_HDR) return NULL;   // size + header overflow
  uint8_t order = size_to_order(size + KMALLOC_BIG_HDR);
  phys_addr_t phys = pmm_buddy_alloc(order);
  if (phys == PMM_INVALID_ADDR) return NULL;

  uint8_t* raw = (uint8_t*)(phys + HHDM_OFFSET);
  *raw = order;
  return (void*)(raw + KMALLOC_BIG_HDR);  // 16-byte aligned (raw is page-aligned)
}

void kfree(void* addr) {
  if (!addr) return;

  if (pmm_is_slab_ptr(addr)) {
    // Phase 4: route through the per-CPU layer.  Fast path is a
    // cmpxchg16b push on this CPU's cpu_slot[cls]; cross-CPU frees
    // hit the page's remote_free Treiber stack.
    slab_pcpu_free(addr);
  } else {
    uint8_t* raw = (uint8_t*)addr - KMALLOC_BIG_HDR;
    uint8_t order = *raw;
    phys_addr_t phys = (phys_addr_t)((virt_addr_t)raw - HHDM_OFFSET);
    pmm_buddy_free(phys, order);
  }
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Regression test for the big-alloc header inconsistency: kmalloc used to
// size the buddy block for `size + 8` but return `raw + 16`, so a request in
// the (2^k - 16, 2^k - 8] band over-ran its block by up to 8 bytes.  Verify
// every big (buddy-backed) allocation's usable span (block - header) covers
// the request.  Each chosen size also prints what the buggy `size + 8` sizing
// would have produced, demonstrating the bug and the fix in one run.
void kheap_overflow_selftest(void) {
    kprintf("[kheap_test] big-alloc header consistency check\n");
    static const size_t sizes[] = { 8184, 8185, 16376, 32760 };
    int failed = 0;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t req = sizes[i];
        uint8_t* p = (uint8_t*)kmalloc(req);
        if (!p) { kprintf("[kheap_test] OOM at size %lu\n", (unsigned long)req); continue; }
        if (pmm_is_slab_ptr(p)) { kfree(p); continue; }   // not the big path
        uint8_t order  = *(p - KMALLOC_BIG_HDR);
        size_t  usable = ((size_t)PAGE_SIZE << order) - KMALLOC_BIG_HDR;
        uint8_t old_order  = size_to_order(req + 8);       // what the buggy code chose
        size_t  old_usable = ((size_t)PAGE_SIZE << old_order) - KMALLOC_BIG_HDR;
        if (old_usable < req)
            kprintf("[kheap_test]   req=%lu: pre-fix(size+8) usable=%lu < req (was a bug); now usable=%lu\n",
                    (unsigned long)req, (unsigned long)old_usable, (unsigned long)usable);
        if (usable < req) {
            kprintf("[kheap_test] FAIL req=%lu order=%u usable=%lu < req\n",
                    (unsigned long)req, order, (unsigned long)usable);
            failed = 1;
        }
        for (size_t j = 0; j < req; j++) p[j] = (uint8_t)0xA5;  // exercise the full write
        kfree(p);
    }
    kprintf(failed ? "[kheap_test] SELF-TEST FAILED (usable < requested)\n"
                   : "[kheap_test] SELF-TEST PASSED (big-alloc usable >= requested)\n");
}
#endif

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


