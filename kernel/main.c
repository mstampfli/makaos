#include "common.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
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
#include "tty.h"
#include "evdev.h"
#include "initcall.h"

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

// ── init_kthread ──────────────────────────────────────────────────────────
// Runs in process context after the scheduler and timer are live.
// Calls do_initcalls_subsys() which runs every INITCALL_SUBSYS registration
// in priority order, then spawns login + dhcpcd.

static void init_kthread(void) {
    ahci_start_io_thread();

    do_initcalls_subsys();

    // Load userspace processes sequentially — ext2 is not thread-safe.
    static const char* envp[] = { "PATH=/bin", "HOME=/root", "TERM=linux", NULL };

    static const char* login_argv[] = { "/bin/login", NULL };
    task_t* login = elf_exec_from_ext2("/bin/login", pid_alloc(), login_argv, envp, NULL);

    static const char* dhcp_argv[] = { "/bin/dhcpcd", NULL };
    task_t* dhcpd = elf_exec_from_ext2("/bin/dhcpcd", pid_alloc(), dhcp_argv, envp, NULL);

    if (login) sched_add(login);
    if (dhcpd) sched_add(dhcpd);
}

// ── kmain ─────────────────────────────────────────────────────────────────
// Minimal boot sequence: BSS clear, serial, boot-info extraction,
// CPU feature setup, then do_initcalls_early() for all early subsystems,
// then hand off to the scheduler.

void kmain(void) {
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    serial_init_and_say();

    extern void* bootinfo_ptr;
    boot_info_t* info = (boot_info_t*)((uint64_t)bootinfo_ptr + HHDM_OFFSET);

    KERNEL_BASE_PHYS     = info->kernel_phys_base;
    KERNEL_SIZE          = (uint64_t)__kernel_end - (uint64_t)__kernel_start;
    LOADER_RESERVED_SIZE = 32ULL * 1024 * 1024;

    // ── CPU feature setup — must happen before any subsystem uses these ───
    // NXE bit in EFER (no-execute pages)
    {
        uint32_t efer_lo, efer_hi;
        __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080U));
        efer_lo |= (1U << 11);
        __asm__ volatile("wrmsr" : : "a"(efer_lo), "d"(efer_hi), "c"(0xC0000080U));
    }
    // PAT (Page Attribute Table): program MSR 0x277 so that PAT entry 1
    // (selected by PWT=1, PCD=0, PAT=0 in the PTE) is write-combining (WC).
    // Default layout: [0]=WB [1]=WT [2]=UC- [3]=UC [4]=WB [5]=WT [6]=UC- [7]=UC
    // We change entry 1 from WT (0x04) to WC (0x01) — only affects pages
    // mapped with PWT=1,PCD=0 which is exclusively the user framebuffer mapping.
    // MMIO mappings use PWT=1,PCD=1 → entry 3 (UC) — unchanged.
    {
        // PAT encoding: each entry is 3 bits in an 8-byte MSR.
        // Byte 0 = entry 0, byte 1 = entry 1, ..., byte 7 = entry 7.
        // Types: WB=0x06, WT=0x04, UC-=0x07, UC=0x00, WC=0x01
        uint32_t pat_lo = 0x00010406; // [3]=UC [2]=UC- [1]=WC [0]=WB  (changed [1] WT→WC)
        uint32_t pat_hi = 0x00070406; // [7]=UC [6]=UC- [5]=WT [4]=WB  (upper half unchanged)
        __asm__ volatile("wrmsr" : : "a"(pat_lo), "d"(pat_hi), "c"(0x277U));
    }
    // SSE/SSE2: CR4.OSFXSR + CR4.OSXMMEXCPT, clear CR0.EM + CR0.TS
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uint64_t)(1 << 2));
        cr0 &= ~((uint64_t)(1 << 3));
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);
        cr4 |= (1ULL << 10);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    // ── Framebuffer — needed by early serial/debug output ────────────────
    fb_init(info->fb_phys, info->fb_width, info->fb_height, info->fb_pitch);

    // ── IDT — must be live before any other init raises exceptions ────────
    idt_init();

    // ── PIC — remap and eventually disable in favour of LAPIC/IOAPIC ─────
    pic_init(0x20, 0x28);

    // ── ACPI — parse tables so lapic/ioapic init can consume g_acpi ──────
    g_acpi = acpi_parse(0);
    if (!g_acpi.ok) {
        g_acpi.lapic_phys      = 0xFEE00000ULL;
        g_acpi.ioapic_phys     = 0xFEC00000ULL;
        g_acpi.ioapic_gsi_base = 0;
        g_acpi.override_count  = 0;
        g_acpi.ok              = 1;
    }

    // ── Memory — PMM then heap then VMM (strict dependency order) ────────
    tsc_init();
    pmm_buddy_init_from_map(info->e820_map, info->e820_count);
    kheap_init();
    vmm_init(info->pml4_phys);

    // ── Interrupt controllers ─────────────────────────────────────────────
    lapic_init(g_acpi.lapic_phys);
    ioapic_init(&g_acpi);
    pic_disable();
    idt_irq_register(VEC_LAPIC_SPURIOUS, (uint64_t)lapic_spurious_entry);

    // ── CPU structures ────────────────────────────────────────────────────
    tss_init();
    syscall_init();

    // ── Scheduler + timer — sched_init must precede timer_init ───────────
    sched_init();
    timer_init(1000);
    __asm__ volatile("sti");

    // ── Early initcalls ───────────────────────────────────────────────────
    do_initcalls_early();

    // ── Hand off to init_kthread (subsys initcalls run there) ────────────
    task_t* kt = task_create_kthread(init_kthread, pid_alloc());
    if (!kt) for (;;) __asm__ volatile("hlt");
    sched_add(kt);

    for (;;)
        __asm__ volatile("hlt");
}
