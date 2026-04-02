#include "common.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "ahci_loader.h"
extern char __bss_start[];
extern char __bss_end[];

/* Add a 1GiB identity-map entry for 3GiB..4GiB (cache-disabled) so that the
 * AHCI ABAR at ~0xFEB00000 is accessible before the kernel's page tables are
 * built.  We navigate the live boot2 page tables via CR3. */
static void map_3gib_region(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    /* PML4[0] → PDPT for low identity map */
    uint64_t* pml4 = (uint64_t*)(cr3 & ~0xFFFULL);
    uint64_t* pdpt = (uint64_t*)(pml4[0] & ~0xFFFULL);

    /* 1GiB huge page, identity, P|RW|PS|PWT|PCD = 0x9B */
    pdpt[3] = (3ULL << 30) | 0x9BU;

    /* Flush TLB by reloading CR3. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void kmain(boot_info_t* info) {
    uint8_t* p = (uint8_t*)info;

    volatile uint32_t r_sig0 = *(uint32_t*)(p + 0);
    volatile uint32_t r_sig1 = *(uint32_t*)(p + 4);
    volatile uint32_t r_sig2 = *(uint32_t*)(p + 8);
    volatile uint32_t r_sig3 = *(uint32_t*)(p + 12);
    volatile uint16_t r_cnt  = *(uint16_t*)(p + 58);

    (void)r_sig0; (void)r_sig1; (void)r_sig2; (void)r_sig3; (void)r_cnt;
    boot_info_t bi = *info;

    /* Zero BSS. */
    for (char *q = __bss_start; q < __bss_end; q++)
        *q = 0;

    idt_init();

    mem_survey_t mem = survey_memory(&bi);
    while (mem.hole == 0) {
        __asm__ __volatile__("hlt");
    }

    /* Extend page tables so AHCI ABAR (3..4 GiB) is identity-mapped. */
    map_3gib_region();

    /* Init AHCI and read kernel from LBA 2048. */
    while (!ahci_loader_init()) {
        __asm__ __volatile__("hlt");
    }

    uint32_t sectors = KERNEL_SIZE_NEEDED / 512;
    uint8_t success = ahci_loader_read(2048, (void*)mem.hole, sectors);

    if (success != 1) {
        while (1) __asm__ __volatile__("hlt");
    }

    setup_paging(&bi, mem.hole, mem.ceiling);

    bi.kernel_phys_base = mem.hole;
    bi.phys_ceiling = mem.ceiling;

    *info = bi;
    return;
}
