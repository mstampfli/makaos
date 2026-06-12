bits 64
global _entry
extern main
extern environ
extern __makaos_tls_init
extern __init_array_start
extern __init_array_end

section .text.entry
_entry:
    ; On entry the kernel has set up the SysV AMD64 initial stack:
    ;   [rsp]          = argc  (uint64_t)
    ;   [rsp+8]        = argv[0] pointer
    ;   ...
    ;   [rsp+8*(argc+1)] = NULL  (end of argv)
    ;   [rsp+8*(argc+2)] = envp[0] pointer
    ;   ...
    ;   NULL           (end of envp)
    ;   auxv pairs follow
    ;
    ; SysV ABI: rdi = arg1, rsi = arg2, rdx = arg3

    mov  rdi, [rsp]        ; argc
    lea  rsi, [rsp+8]      ; argv

    ; Compute envp = argv + argc + 1 (skip argv[] + NULL terminator)
    mov  rcx, rdi          ; rcx = argc
    lea  rdx, [rsi + rcx*8 + 8]  ; envp = &argv[argc+1]

    ; Set the global `environ` before calling main so that code
    ; that reads `environ` (instead of the envp argument) works correctly.
    mov  [rel environ], rdx

    ; SysV ABI: RSP must be 16-byte aligned before the call.
    ; elf_setup_stack already returns a 16-byte aligned RSP.
    and  rsp, ~0xF

    ; Run constructors (.init_array + legacy .ctors — the linker
    ; script collects both into one window).  glib/gobject's type
    ; system boots from a constructor; skipping these left every
    ; statically-linked GLib consumer with NULL type tables (sway
    ; crashed in g_hash_table_lookup_node on first pango use).
    ; Entries of 0 and -1 are list terminators some toolchains emit —
    ; skip them.  rdi/rsi/rdx (argc/argv/envp) are saved around the
    ; loop in callee-saved registers.
    mov  r12, rdi
    mov  r13, rsi
    mov  r14, rdx

    ; TLS before constructors — a constructor (or anything it calls)
    ; may touch errno, which lives at %fs-relative storage now.
    ; Pass envp (r14) so __makaos_tls_init can find auxv (AT_PHDR…)
    ; right after the envp NULL terminator and read PT_TLS exactly.
    mov  rdi, r14
    call __makaos_tls_init

    lea  rbx, [rel __init_array_start]
    lea  r15, [rel __init_array_end]
.ctor_loop:
    cmp  rbx, r15
    jae  .ctors_done
    mov  rax, [rbx]
    add  rbx, 8
    test rax, rax
    jz   .ctor_loop
    cmp  rax, -1
    je   .ctor_loop
    call rax
    jmp  .ctor_loop
.ctors_done:
    mov  rdi, r12
    mov  rsi, r13
    mov  rdx, r14

    call main

    ; main returned — call exit(return_value)
    mov  rdi, rax
    mov  rax, 1            ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
