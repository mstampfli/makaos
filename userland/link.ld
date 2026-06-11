ENTRY(_entry)

PHDRS {
    text PT_LOAD FLAGS(5);   /* r-x */
    data PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS {
    . = 0x400000;
    .text   : { *(.text.entry) *(.text*) } :text
    .rodata : { *(.rodata*) } :text

    /* Constructor tables.  Statically linked glib/gobject (sway's
       text stack) registers its type-system bootstrap with
       __attribute__((constructor)); crt0 walks this window before
       main.  Both the modern .init_array and the legacy .ctors our
       cross-gcc emits land here.  KEEP() — these are referenced by
       symbol window, never by section relocs, so GC would drop them. */
    . = ALIGN(8);
    .init_array : {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        PROVIDE(__init_array_end = .);
    } :text
    . = ALIGN(4096);
    .data   : { *(.data*) } :data
    .bss    : { *(.bss*) *(COMMON) } :data
}
