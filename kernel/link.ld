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

    /* ── Initcall DAG tables ─────────────────────────────────────────────────
     * One section per level.  Each entry is a pointer to an initcall_t.
     * The DAG runner (initcall.c) reads these, builds a dependency graph,
     * topologically sorts it, and executes in the correct order.
     *
     * Level 0 (INITCALL_LEVEL_EARLY)  — runs in kmain, no scheduler
     * Level 1 (INITCALL_LEVEL_SUBSYS) — runs in init_kthread, may sleep
     *
     * KEEP() prevents linker GC.  Sections must also be listed in the
     * objcopy -j flags in build.sh so they appear in the raw binary.
     */
    .initcall_0 : ALIGN(8) {
        __initcall_0_start = .;
        KEEP(*(.initcall_0))
        __initcall_0_end = .;
    }
    . = ALIGN(8);

    .initcall_1 : ALIGN(8) {
        __initcall_1_start = .;
        KEEP(*(.initcall_1))
        __initcall_1_end = .;
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
