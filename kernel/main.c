#include "common.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "keyboard.h"
#include "process.h"
#include "pic.h"
#include "timer.h"
#include "sched.h"
#include "ahci.h"
#include "ext2.h"
#include "tsc.h"
#include "fb.h"

phys_addr_t KERNEL_BASE_PHYS     = 0;
uint64_t    KERNEL_SIZE          = 0;
uint64_t    LOADER_RESERVED_SIZE = 0;

/* ── Process entry points ───────────────────────────────── */
extern void home_fn(void);
extern void snake_fn(void);
extern void fs_init_fn(void);
extern void test_vmalloc_fn(void);
extern void shell_fn(void);

/* ── kmain ───────────────────────────────────────────────── */
static void serial_init_and_say(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, 'K');
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, '\n');
}

void kmain(void) {
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    serial_init_and_say();

    extern void* bootinfo_ptr;
    boot_info_t* info = (boot_info_t*)((uint64_t)bootinfo_ptr + HHDM_OFFSET);

    KERNEL_BASE_PHYS     = info->kernel_phys_base;
    KERNEL_SIZE          = (uint64_t)__kernel_end - (uint64_t)__kernel_start;
    LOADER_RESERVED_SIZE = 32ULL * 1024 * 1024;

    /* Enable NXE bit in EFER */
    {
        uint32_t efer_lo, efer_hi;
        __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080U));
        efer_lo |= (1U << 11);
        __asm__ volatile("wrmsr" : : "a"(efer_lo), "d"(efer_hi), "c"(0xC0000080U));
    }

    fb_init(info->fb_phys, info->fb_width, info->fb_height, info->fb_pitch);
    idt_init();
    pic_init(0x20, 0x28);  // remap PIC early so pic_unmask works in ahci_init
    tsc_init();
    pmm_buddy_init_from_map(info->e820_map, info->e820_count);
    kheap_init();
    vmm_init(info->pml4_phys);
    tss_init();
    syscall_init();

    ahci_init();
    ext2_init(4096);  // ext2 partition starts at LBA 4096 on the single AHCI disk

    task_t* p_shell = process_create(shell_fn, pid_alloc());

    if (!p_shell)
        for (;;) __asm__ volatile("hlt");

    sched_init();
    keyboard_init();
    sched_add(p_shell);
    timer_init(100);

    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}
