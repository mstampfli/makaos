#include "common.h"

typedef uint64_t pte_t;
typedef uint64_t pde_t;
typedef uint64_t pdpte_t;
typedef uint64_t pml4e_t;

typedef struct vmm_page_info_t{
  uint8_t present;
  uint64_t paddr;
  uint64_t flags;
} vmm_page_info_t;

/* Base Hardware Bit Definitions */
#define _MMU_BIT_PRESENT  (1ULL << 0)
#define _MMU_BIT_RW       (1ULL << 1)
#define _MMU_BIT_USER     (1ULL << 2)
#define _MMU_BIT_PWT      (1ULL << 3)
#define _MMU_BIT_PCD      (1ULL << 4)
#define _MMU_BIT_ACCESSED (1ULL << 5)
#define _MMU_BIT_DIRTY    (1ULL << 6)
#define _MMU_BIT_PS       (1ULL << 7)
#define _MMU_BIT_GLOBAL   (1ULL << 8)
#define _MMU_BIT_NX       (1ULL << 63)

#define MMU_ADDR_MASK     0x000FFFFFFFFFF000ULL

/* PML4E Aliases */
#define PML4E_PRESENT   _MMU_BIT_PRESENT
#define PML4E_RW        _MMU_BIT_RW
#define PML4E_USER      _MMU_BIT_USER
#define PML4E_PWT       _MMU_BIT_PWT
#define PML4E_PCD       _MMU_BIT_PCD
#define PML4E_ACCESSED  _MMU_BIT_ACCESSED
#define PML4E_NX        _MMU_BIT_NX

/* PDPE Aliases */
#define PDPE_PRESENT    _MMU_BIT_PRESENT
#define PDPE_RW         _MMU_BIT_RW
#define PDPE_USER       _MMU_BIT_USER
#define PDPE_PWT        _MMU_BIT_PWT
#define PDPE_PCD        _MMU_BIT_PCD
#define PDPE_ACCESSED   _MMU_BIT_ACCESSED
#define PDPE_1G_HUGE    _MMU_BIT_PS
#define PDPE_NX         _MMU_BIT_NX

/* PDE Aliases */
#define PDE_PRESENT     _MMU_BIT_PRESENT
#define PDE_RW          _MMU_BIT_RW
#define PDE_USER        _MMU_BIT_USER
#define PDE_PWT         _MMU_BIT_PWT
#define PDE_PCD         _MMU_BIT_PCD
#define PDE_ACCESSED    _MMU_BIT_ACCESSED
#define PDE_2M_HUGE     _MMU_BIT_PS
#define PDE_NX          _MMU_BIT_NX

/* PTE Aliases */
#define PTE_PRESENT     _MMU_BIT_PRESENT
#define PTE_RW          _MMU_BIT_RW
#define PTE_USER        _MMU_BIT_USER
#define PTE_PWT         _MMU_BIT_PWT
#define PTE_PCD         _MMU_BIT_PCD
#define PTE_ACCESSED    _MMU_BIT_ACCESSED
#define PTE_DIRTY       _MMU_BIT_DIRTY
#define PTE_PAT         _MMU_BIT_PS
#define PTE_GLOBAL      _MMU_BIT_GLOBAL
#define PTE_NX          _MMU_BIT_NX

#define VMM_INVALID UINT64_MAX

#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_NX         (1ULL << 63)
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define VMM_INVALID_PAGE UINT64_MAX

uint8_t vmm_init(void);
void* vmm_page_alloc(virt_addr_t vaddr, uint64_t flags); 
void vmm_page_free(virt_addr_t vaddr, uint64_t flags); 

