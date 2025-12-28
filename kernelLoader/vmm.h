#pragma once
#include "common.h"
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define HHDM_VIRT_BASE   0xFFFF888000000000ULL
#define PTE_P   (1ULL << 0)
#define PTE_RW  (1ULL << 1)
#define PTE_PS  (1ULL << 7)

#define MIB2    (2ULL * 1024ULL * 1024ULL)
#define KERNEL_MAP_SIZE_BYTES (32ULL * 1024ULL * 1024ULL)  // you load 32MiB
#define KERNEL_PDE_COUNT (KERNEL_MAP_SIZE_BYTES / MIB2)    // 16

void setup_paging(boot_info_t* info, uint64_t k_phys, uint64_t p_ceiling); 
