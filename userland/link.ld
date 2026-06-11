ENTRY(_entry)

PHDRS {
    text PT_LOAD FLAGS(5);   /* r-x */
    data PT_LOAD FLAGS(6);   /* rw- */
    tls  PT_TLS  FLAGS(4);   /* TLS template — ld needs this segment to
                                resolve local-exec @tpoff relocations */
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

    /* TLS template.  __makaos_tls_init copies .tdata and zeroes .tbss
       into each thread's block (variant II: %fs points just past it). */
    . = ALIGN(64);
    PROVIDE(__tdata_start = .);
    .tdata  : { *(.tdata .tdata.*) } :data :tls
    PROVIDE(__tdata_end = .);
    .tbss   : { *(.tbss .tbss.*) *(.tcommon) } :tls
    PROVIDE(__tbss_end = .);

    .bss    : { *(.bss*) *(COMMON) } :data
}
