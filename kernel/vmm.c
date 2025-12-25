#include "vmm.h"
#include "pmm.h"

static inline uint64_t read_rsp(void) {
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static inline void invlpg(uint64_t vaddr) {
  asm volatile("invlpg %0" : : "m"(*(char*)vaddr));
}

static inline virt_addr_t vmm_phys_to_virt(phys_addr_t phys) {
  return phys + HHDM_OFFSET;
}

static inline phys_addr_t virt_to_phys(virt_addr_t virt) {
    // Only valid if the address is actually within the HHDM range
    return virt - HHDM_OFFSET;
}

static inline void zero(virt_addr_t addr, uint64_t amount_bytes) {
  uint8_t* ptr = (uint8_t*)addr;
  for (volatile uint64_t i = 0; i < amount_bytes; i++) {
    ptr[i] = 0;
  }
}

static inline uint64_t align_down(uint64_t addr) {
    return (addr & ~(PAGE_SIZE - 1));
}

static inline uint64_t align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static inline void vmm_pml4_load(uint64_t pml4_phys) {
    __asm__ volatile (
        "mov %0, %%cr3"
        : 
        : "r" (pml4_phys)
        : "memory"
    );
}

static inline phys_addr_t vmm_pml4_get() {
  uint64_t cr3;

  // CR3 contains the physical address of the PML4
  asm volatile("mov %%cr3, %0" : "=r"(cr3));

  // Bits 0-11 contain flags (PWT, PCD) or PCID 
  // mask them to get the actual 4KB aligned physical address
  return cr3 & 0x000FFFFFFFFFF000ULL;
}

pte_t* vmm_pte_get(phys_addr_t pml4_phys, virt_addr_t vaddr, uint8_t create) {
  uint8_t levels_shift[] = {39, 30, 21, 12};
  phys_addr_t current_table_phys = pml4_phys;

  for (uint8_t i = 0; i < 3; i++) {
    uint64_t idx = (vaddr >> levels_shift[i]) & 0x1FF;
    uint64_t* table_virt = (uint64_t*)vmm_phys_to_virt(current_table_phys);
    uint64_t entry = table_virt[idx];

    if (!(entry & PAGE_PRESENT)) {
      if (!create) return NULL;

      phys_addr_t new_frame = pmm_frame_alloc();
      if (new_frame == PMM_INVALID_FRAME) return NULL;

      zero(vmm_phys_to_virt(new_frame), 4096);

      entry = new_frame | PAGE_PRESENT | PAGE_WRITABLE;
      table_virt[idx] = entry;
    }
    
    current_table_phys = entry & PAGE_ADDR_MASK;
  }

  uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
  uint64_t* pt_virt = (uint64_t*)vmm_phys_to_virt(current_table_phys);

  return &pt_virt[pt_idx];
}
  
uint8_t vmm_page_map(phys_addr_t pml4_phys, virt_addr_t vaddr, uint64_t paddr, uint64_t flags) {
  pte_t* pte = vmm_pte_get(pml4_phys, vaddr, 1);
  if (pte == NULL) return 0;
  
  *pte = (paddr & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;

  //invalidate cache
  invlpg(vaddr);
  return 1;
}

uint8_t vmm_page_unmap(phys_addr_t pml4_phys, virt_addr_t vaddr, phys_addr_t* out_paddr) {
  pte_t* pte = vmm_pte_get(pml4_phys, vaddr, 0);
  if (pte == NULL) return 0;

  if (out_paddr) {
    *out_paddr = *pte & PAGE_ADDR_MASK; // Extract phys addr before clearing
  }

  *pte = 0ULL;

  //invalidate cache
  invlpg(vaddr);
  return 1;
}

void* vmm_page_alloc(virt_addr_t vaddr, uint64_t flags) {
  phys_addr_t page_frame = pmm_frame_alloc();
  if (page_frame == PMM_INVALID_FRAME) return NULL;

  uint8_t success = vmm_page_map(vmm_pml4_get(), vaddr, page_frame, flags);

  if (!success) return NULL;
  return (void*)vaddr;
}

void vmm_page_free(virt_addr_t vaddr, uint64_t flags) {
  phys_addr_t addr_phys_ptr;
  if (vmm_page_unmap(vmm_pml4_get(), vaddr, &addr_phys_ptr)) 
  pmm_frame_free(addr_phys_ptr >> PAGE_SHIFT);
}

pte_t* vmm_pte_get_offline(virt_addr_t pml4_virt, virt_addr_t vaddr, uint8_t create) {
    uint8_t levels_shift[] = {39, 30, 21, 12};
    uint64_t* current_table_virt = (uint64_t*)pml4_virt;

    for (uint8_t i = 0; i < 3; i++) {
        uint64_t idx = (vaddr >> levels_shift[i]) & 0x1FF;
        uint64_t entry = current_table_virt[idx];

        if (!(entry & PAGE_PRESENT)) {
            if (!create) return NULL;
            phys_addr_t new_frame = pmm_frame_alloc();
            
            // CRITICAL: Since we are in 1:1 mode, 
            // the virtual address is the physical address.
            zero((virt_addr_t)new_frame, 4096);

            entry = new_frame | PAGE_PRESENT | PAGE_WRITABLE;
            current_table_virt[idx] = entry;
        }
        
        // Move to next level. 
        // Again, assuming 1:1 map exists for these new frames.
        current_table_virt = (uint64_t*)(entry & PAGE_ADDR_MASK);
    }

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    return &current_table_virt[pt_idx];
}

void vmm_page_map_internal(virt_addr_t pml4_virt, virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags) {
    // We need a version of vmm_pte_get that doesn't use vmm_pml4_get()
    pte_t* pte = vmm_pte_get_offline(pml4_virt, vaddr, 1);
    if (pte) {
        *pte = (paddr & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;
    }
}

void vmm_kernel_map(virt_addr_t pml4_virt) {
    virt_addr_t v_start = (uint64_t)__kernel_start;
    phys_addr_t p_start = v_start;
    uint64_t size    = (uint64_t)__kernel_end - v_start;

    for (uint64_t i = 0; i < size; i += PAGE_SIZE) {
        vmm_page_map_internal(pml4_virt, v_start + i, p_start + i, PAGE_WRITABLE);
    }
    
    uint64_t rsp = read_rsp();
    uint64_t window = 256 * 1024; // 256 KiB safety margin

    uint64_t start = align_down(rsp - window);
    uint64_t end   = align_up(rsp + PAGE_SIZE); // include current page and one above
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vmm_page_map_internal(pml4_virt, va, va, PAGE_WRITABLE);
    }
}

// Maps a 1GB physical frame to a virtual address by terminating the walk at Level 3
uint8_t vmm_page_1gb_map_offline(virt_addr_t pml4_virt, virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t *pml4 = (uint64_t*)pml4_virt;

    // Ensure PML4 entry exists and points to a PDPT
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t new_pdpt = pmm_frame_alloc();
        if (new_pdpt == PMM_INVALID_FRAME) return 0;
        // Use HHDM to zero the new table before use
        zero(new_pdpt, 4096);
        pml4[pml4_idx] = new_pdpt | PAGE_PRESENT | PAGE_WRITABLE;
    }

    // Traverse to the PDPT
    uint64_t* pdpt_virt = (uint64_t*)(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    // Terminate the walk here by setting the PS bit 
    // The address in the PDPT entry now refers to a 1GiB physical block
    pdpt_virt[pdpt_idx] = (paddr & 0x000FFFFFC0000000ULL) | flags | PAGE_PRESENT | PS_BIT;

    return 1;
}

uint8_t vmm_hhdm_create(virt_addr_t pml4_virt) {
  phys_addr_t max_pa = pmm_highest_address_get();
  
  uint8_t success = 1;
  for (phys_addr_t p = 0; p < max_pa; p += GIB_SIZE) {
    if (!vmm_page_1gb_map_offline(pml4_virt, p + HHDM_OFFSET, p, PAGE_WRITABLE | PAGE_NX)) return 0;
  }

  if (!success) return 0;
  return 1;
}

uint8_t vmm_init(void) {
  phys_addr_t pml4_phys = pmm_frame_alloc();
  if (pml4_phys == PMM_INVALID_FRAME) return 0;

  //early its 1:1 so no conversion here
  virt_addr_t pml4_virt = pml4_phys;
  
  zero(pml4_virt, 4096);
  if (!vmm_hhdm_create(pml4_virt)) return 0;
  

  vmm_kernel_map(pml4_virt);
  vmm_pml4_load(pml4_phys);
  
  return 1;
}

//void vmm_address_space_switch(phys_addr_t pml4_phys) {}

//uint64_t vmm_address_space_create(void) {}

//vmm_page_info_t vmm_query_page(phys_addr_t pml4_phys, virt_addr_t vaddr) {}
  
  

