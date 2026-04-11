ENTRY(_entry)

PHDRS {
    text PT_LOAD FLAGS(5);   /* r-x */
    data PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS {
    . = 0x400000;
    .text   : { *(.text.entry) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data   : { *(.data*) } :data
    .bss    : { *(.bss*) *(COMMON) } :data
}
