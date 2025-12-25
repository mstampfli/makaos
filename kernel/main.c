#include "common.h"
#include "pmm.h"
#include "vmm.h"

void kmain(void) {
    // zero bss
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    
    extern boot_info_t* bootinfo_ptr;
    
    char* msg = "HELLO WORLD!";

    volatile uint16_t* v = (uint16_t*)VGA_ADDR; //VGA
    for (uint32_t i = 0; msg[i] != '\0'; i++) {
        v[i] = (uint16_t)msg[i] | (uint16_t)(0x07 << 8);
    }
    pmm_init(bootinfo_ptr->e820_map, bootinfo_ptr->e820_count);
    vmm_init();

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
