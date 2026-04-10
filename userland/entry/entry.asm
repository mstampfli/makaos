bits 64
global _entry
extern main
extern environ

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

    call main

    ; main returned — call exit(return_value)
    mov  rdi, rax
    mov  rax, 1            ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang
