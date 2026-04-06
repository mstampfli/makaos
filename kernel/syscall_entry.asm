bits 64

extern syscall_dispatch
extern g_tss                  ; TSS struct in tss.c — RSP0 is at byte offset 4
extern g_syscall_user_rsp     ; scratch qword in syscall.c to stash user RSP
extern g_syscall_user_rip     ; scratch qword in syscall.c to stash user RIP
extern g_syscall_user_rflags  ; scratch qword in syscall.c to stash user RFLAGS
extern g_syscall_user_rbp     ; scratch qword in syscall.c to stash user RBP
extern g_syscall_user_rbx     ; scratch qword in syscall.c to stash user RBX
extern g_syscall_user_r12     ; scratch qword in syscall.c to stash user R12
extern g_syscall_user_r13     ; scratch qword in syscall.c to stash user R13
extern g_syscall_user_r14     ; scratch qword in syscall.c to stash user R14
extern g_syscall_user_r15     ; scratch qword in syscall.c to stash user R15
extern g_exec_requested       ; byte flag set by sys_exec
extern g_exec_entry           ; new entry RIP for exec
extern g_exec_rsp             ; new user RSP for exec
extern g_exec_pml4            ; new PML4 phys for exec
extern g_signal_deliver       ; byte: 1=enter handler, 2=sigreturn restore
extern g_signal_rdi           ; uint64_t: signum passed as rdi to handler

global syscall_entry

; ── syscall ABI on entry ───────────────────────────────────────────────────
; rax    = syscall number
; rdi    = arg1,  rsi = arg2,  rdx = arg3,  r10 = arg4
; rcx    = user RIP    (saved by CPU before jumping here)
; r11    = user RFLAGS (saved by CPU before jumping here)
; rsp    = still user stack — must not be used before we switch

; ── sysretq ABI on exit ───────────────────────────────────────────────────
; rcx    = RIP  to restore into user RIP
; r11    = RFLAGS to restore
; rsp    = user RSP
; rax    = return value

syscall_entry:
    ; 1. Save user RSP/RIP/RFLAGS in static scratch variables.
    ;    We cannot push yet — rsp is the user stack.
    mov [rel g_syscall_user_rsp],    rsp
    mov [rel g_syscall_user_rip],    rcx
    mov [rel g_syscall_user_rflags], r11
    ; Save callee-saved regs so fork() can give the child a consistent state.
    mov [rel g_syscall_user_rbp], rbp
    mov [rel g_syscall_user_rbx], rbx
    mov [rel g_syscall_user_r12], r12
    mov [rel g_syscall_user_r13], r13
    mov [rel g_syscall_user_r14], r14
    mov [rel g_syscall_user_r15], r15

    ; 2. Switch to kernel stack (TSS.RSP0, byte offset 4 in tss_t).
    mov rsp, [rel g_tss + 4]

    ; 3. Re-enable interrupts — we're on a safe kernel stack now.
    sti

    ; 4. Push the three values sysretq needs so they survive the C call.
    ;    Push user RSP first (lowest priority, popped last) so that rcx and r11
    ;    can be popped while still on the kernel stack before switching RSP.
    push qword [rel g_syscall_user_rsp] ; user RSP   (pushed first → popped last)
    push r11                            ; user RFLAGS
    push rcx                            ; user RIP   (pushed last → popped first)

    ; 5. Save GPRs the user expects intact that C may clobber.
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; 6. Call dispatcher: syscall_dispatch(nr, arg1, arg2, arg3, arg4)
    ;    SysV calling convention: rdi=1st, rsi=2nd, rdx=3rd, rcx=4th, r8=5th
    ;    syscall ABI:              rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4
    mov  r8,  r10    ; arg4 ← r10 (r10 used in syscall ABI instead of rcx)
    mov  rcx, rdx    ; arg3 ← rdx
    mov  rdx, rsi    ; arg2 ← rsi
    mov  rsi, rdi    ; arg1 ← rdi
    mov  rdi, rax    ; nr   ← rax
    call syscall_dispatch
    ; return value in rax — leave it there for the user

    ; 7. Restore GPRs (reverse order of step 5).
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi

    ; 8. Restore sysretq operands (reverse order of step 4).
    ;    Pop rcx and r11 first while still on the kernel stack, THEN switch RSP.
    pop  rcx         ; user RIP   — still on kernel stack
    pop  r11         ; user RFLAGS — still on kernel stack
    pop  rsp         ; user RSP   — now rsp points at user stack (must be last)

    ; 9. Check if exec was requested (sys_exec sets g_exec_requested = 1).
    ;    If so, override rcx/r11/rsp/cr3 before sysretq.
    cmp byte [rel g_exec_requested], 0
    je  .sysret_normal
    mov byte [rel g_exec_requested], 0
    mov rcx, [rel g_exec_entry]
    mov r11, 0x202              ; RFLAGS: IF=1 + reserved bit
    mov rsp, [rel g_exec_rsp]
    mov rax, [rel g_exec_pml4]
    mov cr3, rax

.sysret_normal:
    ; 10. Check if a user signal handler is being delivered, or sigreturn.
    ;     g_signal_deliver=1: entering handler (override rcx/r11/rsp + rdi=signum)
    ;     g_signal_deliver=2: sigreturn    (override rcx/r11/rsp + all callee-saved)
    ;     At this point rsp is already the user RSP (from pop rsp in step 8).
    ;     The globals g_syscall_user_* hold the new values set by signal code.
    cmp byte [rel g_signal_deliver], 0
    je  .no_signal
    ; Use r10 to read the mode — do NOT touch rax (it holds the syscall return value).
    movzx r10d, byte [rel g_signal_deliver]
    mov byte [rel g_signal_deliver], 0
    mov rcx, [rel g_syscall_user_rip]
    mov r11, [rel g_syscall_user_rflags]
    mov rsp, [rel g_syscall_user_rsp]
    cmp r10d, 1
    jne .signal_restore
    ; Mode 1: handler entry — set rdi = signum (first arg to handler).
    mov rdi, [rel g_signal_rdi]
    jmp .no_signal
.signal_restore:
    ; Mode 2: sigreturn — restore all callee-saved regs so the
    ; interrupted C code sees its original register state.
    mov rbp, [rel g_syscall_user_rbp]
    mov rbx, [rel g_syscall_user_rbx]
    mov r12, [rel g_syscall_user_r12]
    mov r13, [rel g_syscall_user_r13]
    mov r14, [rel g_syscall_user_r14]
    mov r15, [rel g_syscall_user_r15]
.no_signal:
    ; 11. Disable interrupts before sysretq (mandatory).
    cli

    ; 12. Return to user space.
    ;     o64 sysret = sysretq: RIP←rcx, RFLAGS←r11, CS←USER_CS, SS←USER_SS
    o64 sysret
