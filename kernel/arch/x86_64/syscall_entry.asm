bits 64

%include "asm_offsets.inc"    ; CPU_TSS_RSP0 + CPU_SC_* + CPU_SIG_* + CPU_EXEC_*

extern syscall_dispatch

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

; ── Per-CPU scratch ───────────────────────────────────────────────────────
; Pre-9-6 used global scratch variables (g_syscall_user_rsp, etc.).
; Under SMP round-robin those raced: CPU A stored its user RIP,
; CPU B overwrote it with its own, CPU A's sysretq then loaded
; CPU B's RIP and executed in wrong context — observed as a
; kernel #GP inside iretq with a non-canonical RIP.
;
; The scratch now lives per-CPU inside cpu_t.  GS_BASE is programmed
; to &g_cpus[cpu_id] at cpu_init time, so every [gs:CPU_*] access
; lands on the current CPU's slot.  Reads/writes compile to a single
; gs-segment-prefixed mov instruction.  asm_offsets.c generates the
; CPU_SC_* / CPU_SIG_* / CPU_EXEC_* offsets from cpu.h at build time.

syscall_entry:
    ; 1. Save user RSP/RIP/RFLAGS in this CPU's per-CPU scratch slot.
    ;    We cannot push yet — rsp is still the user stack.
    mov [gs:CPU_SC_USER_RSP],    rsp
    mov [gs:CPU_SC_USER_RIP],    rcx
    mov [gs:CPU_SC_USER_RFLAGS], r11
    ; Save callee-saved regs so fork() can give the child a consistent state.
    mov [gs:CPU_SC_USER_RBP], rbp
    mov [gs:CPU_SC_USER_RBX], rbx
    mov [gs:CPU_SC_USER_R12], r12
    mov [gs:CPU_SC_USER_R13], r13
    mov [gs:CPU_SC_USER_R14], r14
    mov [gs:CPU_SC_USER_R15], r15
    ; Save extra syscall arguments (r8=arg5, r9=arg6) for 6-arg syscalls (mmap).
    mov [gs:CPU_SC_ARG5], r8
    mov [gs:CPU_SC_ARG6], r9

    ; 2. Switch to kernel stack (this CPU's TSS.RSP0 — also per-CPU via gs).
    mov rsp, [gs:CPU_TSS_RSP0]

    ; 3. Re-enable interrupts — we're on a safe kernel stack now.
    sti

    ; 4. Push the three values sysretq needs so they survive the C call.
    push qword [gs:CPU_SC_USER_RSP] ; user RSP   (pushed first → popped last)
    push r11                         ; user RFLAGS
    push rcx                         ; user RIP   (pushed last → popped first)

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

    ; ── CRITICAL: from here down we read per-CPU scratch slots
    ; (CPU_SIG_DELIVER, CPU_SC_USER_RIP/RSP/RFLAGS, CPU_EXEC_*) that
    ; belong to THIS syscall's invocation.  If a timer IRQ fires
    ; between these reads and iretq, sched_tick can preempt us,
    ; run an arbitrary other task through its own signal_setup_frame,
    ; which overwrites the very slots we're about to consume.  When
    ; our CPU resumes we'd load the other task's rip/rsp into rcx/rsp
    ; and iretq into a non-canonical RIP belonging to a different
    ; process — observed as #GP at iretq with RIP=0x481374C985480000
    ; during Ctrl+C SIGINT delivery to makaterm (the foreign payload
    ; there disassembles to test rcx,rcx / jz, i.e. some other task's
    ; real code).  Keep interrupts off for the entire consume window.
    cli

    ; 8. Restore sysretq operands (reverse order of step 4).
    pop  rcx         ; user RIP   — still on kernel stack
    pop  r11         ; user RFLAGS — still on kernel stack
    pop  rsp         ; user RSP   — now rsp points at user stack (must be last)

    ; 9. Check if exec was requested (sys_exec sets exec_requested = 1).
    cmp byte [gs:CPU_EXEC_REQUESTED], 0
    je  .sysret_normal
    mov byte [gs:CPU_EXEC_REQUESTED], 0
    mov rcx, [gs:CPU_EXEC_ENTRY]   ; new RIP
    mov r11, 0x202                  ; new RFLAGS: IF=1 + reserved
    mov rsp, [gs:CPU_EXEC_RSP]     ; new RSP
    mov rax, [gs:CPU_EXEC_PML4]
    mov cr3, rax                    ; switch address space
    ; Clear all GPRs except rcx (RIP) and r11 (RFLAGS) so the new process
    ; does not inherit kernel values — specifically RAX which just held the
    ; PML4 physical address and would be used as a pointer by the entry code.
    xor  eax, eax
    xor  ebx, ebx
    xor  edx, edx
    xor  esi, esi
    xor  edi, edi
    xor  ebp, ebp
    xor  r8d,  r8d
    xor  r9d,  r9d
    xor  r10d, r10d
    xor  r12d, r12d
    xor  r13d, r13d
    xor  r14d, r14d
    xor  r15d, r15d

.sysret_normal:
    ; 10. Check if a user signal handler is being delivered, or sigreturn.
    cmp byte [gs:CPU_SIG_DELIVER], 0
    je  .no_signal
    movzx r10d, byte [gs:CPU_SIG_DELIVER]
    mov byte [gs:CPU_SIG_DELIVER], 0
    mov rcx, [gs:CPU_SC_USER_RIP]
    mov r11, [gs:CPU_SC_USER_RFLAGS]
    mov rsp, [gs:CPU_SC_USER_RSP]
    cmp r10d, 1
    jne .signal_restore
    ; Mode 1: handler entry — set rdi = signum (first arg to handler).
    mov rdi, [gs:CPU_SIG_RDI]
    jmp .no_signal
.signal_restore:
    ; Mode 2: sigreturn — restore all callee-saved regs so the
    ; interrupted C code sees its original register state.
    mov rbp, [gs:CPU_SC_USER_RBP]
    mov rbx, [gs:CPU_SC_USER_RBX]
    mov r12, [gs:CPU_SC_USER_R12]
    mov r13, [gs:CPU_SC_USER_R13]
    mov r14, [gs:CPU_SC_USER_R14]
    mov r15, [gs:CPU_SC_USER_R15]
.no_signal:
    ; (cli already executed at top of consume window — do not re-cli.)

    ; 12. Return to user space via iretq (sysretq has a KVM bug where
    ;     SS doesn't get RPL=3 OR'd in, giving SS=0x20 instead of 0x23).
    mov r10, rsp            ; save user RSP in r10
    mov rsp, [gs:CPU_TSS_RSP0] ; switch to kstack top (this CPU's TSS.RSP0)

    push qword 0x23         ; SS = user data64 with RPL=3
    push r10                 ; RSP = saved user RSP
    push r11                 ; RFLAGS = user RFLAGS
    push qword 0x2B          ; CS = user code64 with RPL=3
    push rcx                 ; RIP = user RIP

    iretq
