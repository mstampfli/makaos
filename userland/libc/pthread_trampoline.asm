; ── pthread_trampoline ─────────────────────────────────────────────────
; Entry point every pthread jumps to after sys_thread returns in the
; child.  The kernel's thread(entry, stack_top, flags) syscall can't
; pass a rdi argument, so pthread_create pushes `start_fn` and `arg`
; on the new stack and launches into us.
;
; Stack layout on entry (after iretq, rsp = stack_top set by
; sys_thread):
;     [rsp + 0]  start_fn    (void* (*)(void*))
;     [rsp + 8]  arg          (void*)
;     [rsp + 16] pad / red-zone
;
; We read them, call start_fn(arg), then jump to pthread_exit with
; the return value in rdi.  Never returns — pthread_exit does the
; SYS_EXIT.

bits 64
global pthread_trampoline
extern pthread_exit

pthread_trampoline:
    pop  rax                ; start_fn
    pop  rdi                ; arg
    call rax                ; rax = start_fn(arg) → return value
    mov  rdi, rax           ; retval → pthread_exit arg
    call pthread_exit
    ; pthread_exit is __attribute__((noreturn)); should never get here.
    ud2
