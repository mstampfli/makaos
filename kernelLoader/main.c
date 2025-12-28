#include "common.h"
#include "idt.h"
#include "ata_driver.h"
#include "pmm.h"
#include "vmm.h"
extern char __bss_start[];
extern char __bss_end[];

void kmain(boot_info_t* info) {
    uint8_t* p = (uint8_t*)info;

    volatile uint32_t r_sig0 = *(uint32_t*)(p + 0);
    volatile uint32_t r_sig1 = *(uint32_t*)(p + 4);
    volatile uint32_t r_sig2 = *(uint32_t*)(p + 8);
    volatile uint32_t r_sig3 = *(uint32_t*)(p + 12);
    volatile uint16_t r_cnt  = *(uint16_t*)(p + 58);

    (void)r_sig0; (void)r_sig1; (void)r_sig2; (void)r_sig3; (void)r_cnt; 
    boot_info_t bi = *info;
    // zero bss
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    
    idt_init();

    //extern boot_info_t* bootinfo_ptr;

    mem_survey_t mem = survey_memory(&bi);
    while (mem.hole == 0) {
        __asm__ __volatile__("hlt");
    }

    for (uint8_t* p = (uint8_t*)0x00400000; p < (uint8_t*)0x00400000 + 256*512; p++) *p = 0xCC;
    volatile uint8_t debug_code = 0;
    //while (!ata_init()) continue;
    //uint8_t success = ata_disk_write_48_poll(2048, (void*)0x00400000, 256);  
    uint32_t sectors = KERNEL_SIZE_NEEDED / 512;
    initializeATA();
    uint8_t success = readFromDiskATA48(2048, (void*)mem.hole, sectors); //32 MB
                                                                         
    if (success != 1) {
        debug_code = success; // Capture error 2, 3, or 4
        while (1) {
            __asm__ __volatile__("hlt");
        }
    }
    debug_code = 0xFF;
    setup_paging(&bi, mem.hole, mem.ceiling);
    
    bi.kernel_phys_base = mem.hole;
    bi.phys_ceiling = mem.ceiling;
    
    *info = bi;
    return;
}
