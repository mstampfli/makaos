; ── pthread_trampoline ─────────────────────────────────────────────────
; Entry point every pthread jumps to after sys_thread returns in the
; child.  The kernel's thread(entry, stack_top, flags) syscall can't
; pass a rdi argument, so pthread_create pushes start_fn/arg/tls_tp on
; the new stack and launches into us.
;
; Stack layout on entry (after iretq, rsp = stack_top set by sys_thread):
;     [rsp + 0]  start_fn    (void* (*)(void*))
;     [rsp + 8]  arg          (void*)
;     [rsp + 16] tls_tp       (thread pointer for SYS_SET_FS)
;     [rsp + 24] descriptor   (struct __pthread* — the pthread_t handle)
;
; Sequence: install TLS, then call __pthread_thread_setup(descriptor)
; which caches the thread's descriptor in %fs (g_self) and registers the
; cleartid join word with the kernel.  pthread_t IS that descriptor
; pointer, so join/self/detach are pointer dereferences — no lookup.
; Then call start_fn(arg) and hand the result to pthread_exit.
; r12/r13 are callee-saved, preserved across the C call.

bits 64
global pthread_trampoline
extern pthread_exit
extern __pthread_thread_setup

pthread_trampoline:
    pop  r12                ; start_fn
    pop  r13                ; arg
    pop  rdi                ; tls_tp
    mov  rax, 115           ; SYS_SET_FS — TLS live before any C code
    syscall
    pop  rdi                ; descriptor → arg to __pthread_thread_setup
    call __pthread_thread_setup   ; cache g_self + SYS_SET_CLEARTID
    mov  rdi, r13
    call r12                ; rax = start_fn(arg) → return value
    mov  rdi, rax           ; retval → pthread_exit arg
    call pthread_exit
    ; pthread_exit is __attribute__((noreturn)); should never get here.
    ud2
