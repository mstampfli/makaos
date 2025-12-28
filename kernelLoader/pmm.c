#include "pmm.h"
#include "common.h"

mem_survey_t survey_memory(boot_info_t* info) {
    mem_survey_t result = {0, 0};
    const uint64_t needed = 32ULL * 1024ULL * 1024ULL;

    for (uint16_t i = 0; i < info->e820_count; i++) {
        uint64_t b = info->e820_map[i].base;
        uint64_t l = info->e820_map[i].length;
        uint64_t e = b + l;

        if (e > result.ceiling) result.ceiling = e;
        if (info->e820_map[i].type != 1) continue;

        // Clip the entry if it starts below 1MB (don't touch the loader/stack)
        uint64_t actual_start = (b < 0x100000ULL) ? 0x100000ULL : b;
        
        // Align the candidate address to a 2MB boundary
        uint64_t candidate = (actual_start + 0x1FFFFFULL) & ~0x1FFFFFULL;

        // Check if there is still enough space between 'candidate' and 'e'
        if (candidate < e && (e - candidate) >= needed) {
            if (result.hole == 0) {
                result.hole = candidate;
                // Don't break, keep looping to find the actual max ceiling
            }
        }
    }
    return result;
}
