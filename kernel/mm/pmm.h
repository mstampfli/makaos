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

typedef struct slab_header_t {
  slab_cache_t* cache;     // owning cache
  phys_addr_t   slab_phys; // base physical address of the slab
  void*         freelist;  // intrusive free-slot stack
  uint32_t      inuse;     // number of allocated objects

  struct slab_header_t* prev;
  struct slab_header_t* next;

  uint8_t in_full_list;    // 0 = partial list, 1 = full list
} slab_header_t;

// partial/full are pointers to slab_header_t nodes embedded in slab pages.
// kept as void* to avoid exposing slab_header_t in the public header.
typedef struct slab_cache_t {
  size_t  slot_size;    // size of one object/slot in bytes
  void*   partial;      // list of slabs with free slots (slab_header_t*)
  void*   full;         // list of slabs with no free slots (slab_header_t*)
  uint8_t slab_order;   // buddy order used to allocate slabs (0 = 1 page)
} slab_cache_t;

phys_addr_t pmm_highest_address_get(void);

mem_survey_t pmm_mem_survey(e820_entry_t* map, uint32_t count);

uint8_t calculate_max_order(uintptr_t addr, uintptr_t end);
uint8_t calculate_slab_order(size_t slot_size);

void pmm_buddy_init_from_map(e820_entry_t* map, uint32_t count);
phys_addr_t pmm_buddy_alloc(uint8_t order);
void pmm_buddy_free(phys_addr_t addr, uint8_t order);

void pmm_slab_cache_init(slab_cache_t* cache, size_t slot_size);
void* pmm_slab_alloc(slab_cache_t* cache);
void pmm_slab_free(void* ptr);
uint8_t pmm_is_slab_ptr(void* ptr);
