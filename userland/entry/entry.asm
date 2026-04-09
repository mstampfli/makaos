bits 64
global _entry
extern main

section .text.entry
_entry:
    ; On entry the kernel has set up the SysV AMD64 initial stack:
    ;   [rsp]      = argc  (uint64_t)
    ;   [rsp+8]    = argv[0] pointer
    ;   ...
    ;   [rsp+8*(argc+1)] = NULL  (end of argv)
    ;   ...envp, auxv follow
    ;
    ; SysV ABI: rdi = arg1, rsi = arg2, align rsp to 16 before call.

    mov  rdi, [rsp]        ; argc
    lea  rsi, [rsp+8]      ; argv

    ; SysV ABI: RSP must be 16-byte aligned before the call.
    ; elf_setup_stack already returns a 16-byte aligned RSP.
    ; Just align down — the call instruction pushes the 8-byte return address.
    and  rsp, ~0xF

    call main

    ; main returned — call exit(return_value)
    mov  rdi, rax
    mov  rax, 1            ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang
