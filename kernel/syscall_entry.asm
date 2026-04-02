bits 64

extern syscall_dispatch
extern g_tss          ; TSS struct in tss.c — RSP0 is at byte offset 4
extern g_syscall_user_rsp ; scratch qword in syscall.c to stash user RSP

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
    ; 1. Save user RSP in a static scratch variable.
    ;    We cannot push yet — rsp is the user stack.
    mov [rel g_syscall_user_rsp], rsp

    ; 2. Switch to kernel stack (TSS.RSP0, byte offset 4 in tss_t).
    mov rsp, [rel g_tss + 4]

    ; 3. Re-enable interrupts — we're on a safe kernel stack now.
    sti

    ; 4. Push the three values sysretq needs so they survive the C call.
    push rcx                            ; user RIP   (rcx on entry)
    push r11                            ; user RFLAGS
    push qword [rel g_syscall_user_rsp] ; user RSP

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
    pop  rsp         ; user RSP  — now rsp points at user stack again
    pop  r11         ; user RFLAGS
    pop  rcx         ; user RIP

    ; 9. Disable interrupts before sysretq (mandatory).
    cli

    ; 10. Return to user space.
    ;     o64 sysret = sysretq: RIP←rcx, RFLAGS←r11, CS←USER_CS, SS←USER_SS
    o64 sysret
