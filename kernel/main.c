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
#include "tsc.h"
#include "fb.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "net/net.h"

phys_addr_t KERNEL_BASE_PHYS     = 0;
uint64_t    KERNEL_SIZE          = 0;
uint64_t    LOADER_RESERVED_SIZE = 0;

/* ── Process entry points ───────────────────────────────── */
extern void shell_fn(void);

/* ── Spurious LAPIC vector handler ──────────────────────── */
extern void lapic_spurious_entry(void);

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

    // ── Interrupt controller bringup ─────────────────────────────────────
    //
    // Order matters:
    //  1. Remap the legacy 8259 PIC so its vectors don't collide with CPU
    //     exceptions (0x00–0x1F).  We do this even though we're about to
    //     disable the PIC, because spurious PIC interrupts can still fire
    //     during the transition window and we need them to hit safe vectors.
    //  2. Parse ACPI MADT to find LAPIC and IOAPIC addresses.
    //  3. Init LAPIC (enables the local CPU's APIC).
    //  4. Init IOAPIC (routes ISA IRQs → LAPIC vectors, masks everything else).
    //  5. Disable the legacy PIC (mask all lines — it's now bypassed).
    //  6. Register the LAPIC spurious vector in the IDT.

    pic_init(0x20, 0x28);   // remap PIC: IRQ0→0x20, IRQ8→0x28

    // ACPI: OVMF passes the RSDP via the EFI configuration table.
    // Our bootloader doesn't forward it yet, so pass 0 and let acpi_parse()
    // search the legacy BIOS area (works for both SeaBIOS and OVMF on QEMU).
    acpi_info_t acpi = acpi_parse(0);

    tsc_init();           // calibrate TSC (uses PIT ch2, no IRQs needed)

    pmm_buddy_init_from_map(info->e820_map, info->e820_count);
    kheap_init();
    vmm_init(info->pml4_phys);

    // LAPIC and IOAPIC require vmm_map_mmio, so init after vmm_init.
    if (acpi.ok) {
        lapic_init(acpi.lapic_phys);
        ioapic_init(&acpi);
    } else {
        // Fallback: use default LAPIC/IOAPIC addresses (works on QEMU).
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

    pic_disable();   // mask all PIC lines — IOAPIC takes over ISA IRQs

    // Register spurious LAPIC vector so a cancelled interrupt doesn't #GP.
    idt_irq_register(VEC_LAPIC_SPURIOUS, (uint64_t)lapic_spurious_entry);

    tss_init();
    syscall_init();

    ahci_init();
    ext2_init(4096);  // ext2 partition starts at LBA 4096 on the single AHCI disk

    task_t* p_shell = process_create(shell_fn, pid_alloc());

    if (!p_shell)
        for (;;) __asm__ volatile("hlt");

    sched_init();
    keyboard_init();
    mouse_init();
    hda_init();
    net_init();
    sched_add(p_shell);

    timer_init(100);   // 100 Hz scheduler tick via LAPIC timer

    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}
