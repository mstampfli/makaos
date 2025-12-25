ENTRY(_start)

SECTIONS
{
    /* Kernel link/virtual address */
    . = 0x400000;

    __kernel_start = .;

    .text : ALIGN(4096) {
        *(.text*)
    }
    . = ALIGN(4096);

    .rodata : ALIGN(4096) {
        *(.rodata*)
    }
    . = ALIGN(4096);

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
