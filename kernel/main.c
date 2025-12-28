#include "common.h"
#include "pmm.h"
#include "vmm.h"

void kmain(void) {
    // zero bss
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    
    /*char* msg = "HELLO WORLD!";

    volatile uint16_t* v = (uint16_t*)(VGA_ADDR); //VGA
    for (uint32_t i = 0; msg[i] != '\0'; i++) {
        v[i] = (uint16_t)msg[i] | (uint16_t)(0x07 << 8);
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }*/
    
    extern void* bootinfo_ptr; //phys
    boot_info_t* info = (boot_info_t*)((uint64_t)bootinfo_ptr + HHDM_OFFSET);

    idt_init();
    pmm_init(info->e820_map, info->e820_count);
    vmm_init(info);

    char* msg1 = "HELLO WORLD1!";

    volatile uint16_t* v1 = (uint16_t*)(VGA_ADDR + HHDM_OFFSET); //VGA
    for (uint32_t i = 0; msg1[i] != '\0'; i++) {
        v1[i] = (uint16_t)msg1[i] | (uint16_t)(0x07 << 8);
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
