#include "vmm.h"

static inline void zero(virt_addr_t addr, uint64_t amount_bytes) {
  uint8_t* ptr = (uint8_t*)addr;
  for (volatile uint64_t i = 0; i < amount_bytes; i++) {
    ptr[i] = 0;
  }
}

void setup_paging(boot_info_t* info, uint64_t k_phys, uint64_t p_ceiling) {
    //  - PML4
    //  - PDPT for low identity
    //  - PD   for low identity (2MiB pages)
    //  - PDPT for higher-half kernel
    //  - PD   for higher-half kernel (2MiB pages)
    //  - PDPT for HHDM (1GiB pages)
    //
    // Total: 6 pages = 24KiB
    uint64_t table_base = k_phys + (32 * 1024 * 1024) - (6 * 4096);

    uint64_t* pml4      = (uint64_t*)(table_base + 0x0000);
    uint64_t* pdpt_id   = (uint64_t*)(table_base + 0x1000);
    uint64_t* pd_id     = (uint64_t*)(table_base + 0x2000);
    uint64_t* pdpt_hh   = (uint64_t*)(table_base + 0x3000);
    uint64_t* pd_kern   = (uint64_t*)(table_base + 0x4000);
    uint64_t* pdpt_hhdm = (uint64_t*)(table_base + 0x5000);

    zero((virt_addr_t)pml4, 6 * 4096);

    // Identity map first 1GiB using 2MiB pages
    pml4[0] = (uint64_t)pdpt_id | (PTE_P | PTE_RW);
    pdpt_id[0] = (uint64_t)pd_id | (PTE_P | PTE_RW);

    // 1GiB / 2MiB = 512 PDEs
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t phys = i * MIB2;
        pd_id[i] = phys | (PTE_P | PTE_RW | PTE_PS); // 2MiB page
    }

    // Higher-half kernel map at 0xFFFFFFFF80000000 using 2MiB pages
    // PML4[511] = higher-half
    // PDPT index 510 corresponds to 0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF
    pml4[511] = (uint64_t)pdpt_hh | (PTE_P | PTE_RW);
    pdpt_hh[510] = (uint64_t)pd_kern | (PTE_P | PTE_RW);

    // k_phys MUST be 2MiB aligned for PS=2MiB PDEs (your survey_memory does that)
    for (uint64_t i = 0; i < KERNEL_PDE_COUNT; i++) {
        uint64_t phys = k_phys + i * MIB2;
        pd_kern[i] = phys | (PTE_P | PTE_RW | PTE_PS); // 2MiB page
    }

    // HHDM using 1GiB pages
    uint16_t hhdm_pml4_idx = (HHDM_OFFSET >> 39) & 0x1FF;
    pml4[hhdm_pml4_idx] = (uint64_t)pdpt_hhdm | (PTE_P | PTE_RW);

    for (uint64_t p = 0; p < p_ceiling; p += GIB_SIZE) {
        uint64_t pdpt_idx = p / GIB_SIZE;
        if (pdpt_idx >= 512) break;
        pdpt_hhdm[pdpt_idx] = p | (PTE_P | PTE_RW | PTE_PS); // 1GiB page
    }

    info->pml4_phys = (uint64_t)pml4;
    info->hhdm_offset = HHDM_OFFSET;
}
