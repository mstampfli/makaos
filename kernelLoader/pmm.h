#pragma once
#include "common.h"

#define MAX_RAM_BYTES (64ULL * 1024 * 1024 * 1024)
#define MAX_FRAMES (MAX_RAM_BYTES / PAGE_SIZE)
#define BITMAP_BYTES ((MAX_FRAMES + 7) / 8)
#define PMM_INVALID_FRAME UINT64_MAX
#define PMM_RESERVED_FRAMES ((64 * 1024 * 1024) >> PAGE_SHIFT)
#define KERNEL_SIZE_NEEDED (32ULL * 1024ULL * 1024ULL) //32MB

typedef struct {
    uint64_t hole;
    uint64_t ceiling;
} mem_survey_t;

mem_survey_t survey_memory(boot_info_t* info); 
