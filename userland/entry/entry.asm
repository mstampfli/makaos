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

    ; Align stack: rsp must be 16-byte aligned *before* the call instruction
    ; pushes the return address. We entered with rsp pointing at argc, so
    ; push a dummy return address slot to make the call site 16-byte aligned.
    and  rsp, ~0xF
    sub  rsp, 8            ; realign so call makes rsp 16-byte aligned

    call main

    ; main returned — call exit(return_value)
    mov  rdi, rax
    mov  rax, 1            ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang
