bits 64
global _entry
extern _start

section .text.entry
_entry:
    call _start
    ; if _start returns, call sys_exit(1)
    mov rdi, 1
    mov rax, 1      ; SYS_EXIT
    syscall
.hang:
    hlt
    jmp .hang
