#include "common.h"
#include "idt.h"
#include "ata_driver.h"
extern char __bss_start[];
extern char __bss_end[];

void kmain(void) {
    // zero bss
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    
    idt_init();

    for (uint8_t* p = (uint8_t*)0x00400000; p < (uint8_t*)0x00400000 + 256*512; p++) *p = 0xCC;
    
    while (!ata_init()) continue;
    uint8_t success = ata_disk_write_48_poll(2048, (void*)0x00400000, 256);  
      
    while (!success) {
        __asm__ __volatile__("hlt");
    }

    return;
}
