#include "common.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "keyboard.h"
#include "mouse.h"
#include "hda.h"
#include "process.h"
#include "pic.h"
#include "timer.h"
#include "sched.h"
#include "ahci.h"
#include "ext2.h"
#include "elf.h"
#include "tsc.h"
#include "fb.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "net/net.h"
#include "tty.h"
#include "evdev.h"

phys_addr_t KERNEL_BASE_PHYS     = 0;
uint64_t    KERNEL_SIZE          = 0;
uint64_t    LOADER_RESERVED_SIZE = 0;

extern void lapic_spurious_entry(void);

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

    /* Enable SSE/SSE2 for userland: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10),
       and clear CR0.EM (bit 2) / CR0.TS (bit 3) so SSE doesn't trap. */
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uint64_t)(1 << 2));  // clear EM
        cr0 &= ~((uint64_t)(1 << 3));  // clear TS
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);   // OSFXSR
        cr4 |= (1ULL << 10);  // OSXMMEXCPT
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    pic_init(0x20, 0x28);

    acpi_info_t acpi = acpi_parse(0);

    tsc_init();

    pmm_buddy_init_from_map(info->e820_map, info->e820_count);
    kheap_init();
    vmm_init(info->pml4_phys);

    if (acpi.ok) {
        lapic_init(acpi.lapic_phys);
        ioapic_init(&acpi);
    } else {
        acpi_info_t fallback = {
            .lapic_phys      = 0xFEE00000ULL,
            .ioapic_phys     = 0xFEC00000ULL,
            .ioapic_gsi_base = 0,
            .override_count  = 0,
            .ok              = 1,
        };
        lapic_init(fallback.lapic_phys);
        ioapic_init(&fallback);
    }

    pic_disable();
    idt_irq_register(VEC_LAPIC_SPURIOUS, (uint64_t)lapic_spurious_entry);

    tss_init();
    syscall_init();

    ahci_init();
    ext2_init(4096);

    // ── Launch /bin/shell as PID 1 ────────────────────────────────────────
    static const char* sh_argv[] = { "/bin/shell", NULL };
    static const char* sh_envp[] = { "PATH=/bin", "HOME=/", "TERM=linux", NULL };
    task_t* init = elf_exec_from_ext2("/bin/shell", pid_alloc(), sh_argv, sh_envp, NULL);
    if (!init)
        for (;;) __asm__ volatile("hlt");  // /bin/shell missing — halt

    sched_init();
    tty_init();
    evdev_init();
    keyboard_init();
    mouse_init();
    hda_init();
    net_init();
    sched_add(init);

    timer_init(100);

    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}
