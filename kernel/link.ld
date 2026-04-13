ENTRY(_start)

SECTIONS
{
    /* Kernel link/virtual address */
    . = 0xFFFFFFFF80000000;

    __kernel_start = .;

    .text : ALIGN(4096) {
        *(.text*)
    }
    . = ALIGN(4096);

    .rodata : ALIGN(4096) {
        *(.rodata*)
    }
    . = ALIGN(4096);

    /* ── Initcall tables ────────────────────────────────────────────────────
     * Two levels, each with 8 priority slots (0 = first, 7 = last).
     * Macros in initcall.h place function pointers into these sections;
     * do_initcalls() walks them in order at boot.
     *
     * INITCALL_EARLY  — runs in kmain, interrupts off, no scheduler
     * INITCALL_SUBSYS — runs in init_kthread, scheduler live
     */
    .initcall_early : ALIGN(8) {
        __initcall_early_start = .;
        KEEP(*(.initcall_early0)) KEEP(*(.initcall_early1))
        KEEP(*(.initcall_early2)) KEEP(*(.initcall_early3))
        KEEP(*(.initcall_early4)) KEEP(*(.initcall_early5))
        KEEP(*(.initcall_early6)) KEEP(*(.initcall_early7))
        __initcall_early_end = .;
    }
    . = ALIGN(8);

    .initcall_subsys : ALIGN(8) {
        __initcall_subsys_start = .;
        KEEP(*(.initcall_subsys0)) KEEP(*(.initcall_subsys1))
        KEEP(*(.initcall_subsys2)) KEEP(*(.initcall_subsys3))
        KEEP(*(.initcall_subsys4)) KEEP(*(.initcall_subsys5))
        KEEP(*(.initcall_subsys6)) KEEP(*(.initcall_subsys7))
        __initcall_subsys_end = .;
    }
    . = ALIGN(8);

    .data : ALIGN(4096) {
        *(.data*)
    }
    . = ALIGN(4096);

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }
    . = ALIGN(4096);

    __kernel_end = .;
}
