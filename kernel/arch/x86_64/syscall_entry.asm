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

    ; NOTE: interrupts stay OFF until every per-CPU scratch slot has
    ; been copied onto the per-task kernel stack (end of step 5).  An
    ; IRQ in this window could preempt + migrate the task; the next
    ; read of [gs:CPU_SC_*] would then see a DIFFERENT task's values.

    ; 3b. Push user callee-saved registers onto the kernel stack.  The
    ;     x86-64 syscall ABI requires the kernel preserve rbx/rbp/r12-r15
    ;     across a syscall.  We *also* stash them in per-CPU scratch
    ;     (step 1) so signal_setup_frame can read them — but the per-CPU
    ;     slot is stale if the task migrates CPUs during a blocking
    ;     dispatch.  The kernel stack is per-task, so values pushed here
    ;     follow the task across migrations and are guaranteed to match
    ;     what we promised user space on return.
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

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

    ; The per-task snapshot is complete — the pushes above (steps
    ; 3b/4/5) are exactly what C reads through SYSCALL_KFRAME().
    ; Safe to take interrupts/preemption/migration from here on.
    sti

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

    ; 8. Unwind the kernel stack IN THE ORDER WE PUSHED.
    ;    Pop order: step 4 values (user_rip/rflags/rsp) first, then
    ;    step 3b values (callee-saves).  User RSP goes into r10 (not
    ;    rsp) because the callee-save pops below still need the kernel
    ;    stack pointer.
    pop rcx           ; user RIP
    pop r11           ; user RFLAGS
    pop r10           ; user RSP  (held in r10 until the iretq push)

    ; 8b. Pop the user callee-saves we stashed at step 3b.  Kernel
    ;     stack is per-task, so these follow the task across CPU
    ;     migrations — guaranteed correct regardless of which CPU
    ;     the dispatcher returned on.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; 9. Check if exec was requested (sys_exec sets exec_requested = 1).
    cmp byte [gs:CPU_EXEC_REQUESTED], 0
    je  .sysret_normal
    mov byte [gs:CPU_EXEC_REQUESTED], 0
    mov rcx, [gs:CPU_EXEC_ENTRY]    ; new RIP
    mov r11, 0x202                   ; new RFLAGS: IF=1 + reserved
    mov r10, [gs:CPU_EXEC_RSP]      ; new RSP (into r10 for the shared iretq push)
    mov rax, [gs:CPU_EXEC_PML4]
    mov cr3, rax                     ; switch address space
    ; Clear all GPRs except rcx (RIP), r10 (user RSP) and r11 (RFLAGS)
    ; so the new process does not inherit kernel values — specifically
    ; RAX which just held the PML4 physical address.
    xor  eax, eax
    xor  ebx, ebx
    xor  edx, edx
    xor  esi, esi
    xor  edi, edi
    xor  ebp, ebp
    xor  r8d,  r8d
    xor  r9d,  r9d
    xor  r12d, r12d
    xor  r13d, r13d
    xor  r14d, r14d
    xor  r15d, r15d

.sysret_normal:
    ; 10. (removed) Signal delivery / sigreturn no longer branch here:
    ;     signal_setup_frame and sys_sigreturn mutate the saved values
    ;     on the per-task KERNEL STACK — the very slots steps 7/8/8b
    ;     popped — so the normal unwind already carries the redirected
    ;     rip/rsp/rflags/rdi and restored callee-saves.  Per-task state
    ;     migrates with the task; the per-CPU CPU_SIG_*/CPU_SC_* scratch
    ;     this replaced handed another task's user RSP to bash's
    ;     SIGWINCH frame after a mid-syscall CPU migration.
.no_signal:
    ; (cli already executed at top of consume window — do not re-cli.)

    ; 12. Return to user space via iretq (sysretq has a KVM bug where
    ;     SS doesn't get RPL=3 OR'd in, giving SS=0x20 instead of 0x23).
    mov rsp, [gs:CPU_TSS_RSP0]  ; switch to kstack top for iretq push

    push qword 0x23              ; SS = user data64 with RPL=3
    push r10                     ; RSP = user RSP (in r10 all along)
    push r11                     ; RFLAGS = user RFLAGS
    push qword 0x2B              ; CS = user code64 with RPL=3
    push rcx                     ; RIP = user RIP

    iretq
