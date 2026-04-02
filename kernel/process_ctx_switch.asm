; context_switch(cpu_ctx_t* from, cpu_ctx_t* to, phys_addr_t new_pml4)
;                rdi               rsi            rdx
;
; Saves the current execution context into *from, then restores *to's
; context and jumps back into it.
;
; Why these registers?  System V AMD64 ABI callee-saved set:
;   rbx, rbp, r12, r13, r14, r15 — the callee must preserve these.
;   rsp                           — saved/restored explicitly via cpu_ctx_t.
;
; rip is NOT stored as a field.  Instead, the `call context_switch`
; instruction pushes the return address onto the caller's stack, and we
; capture that implicitly by saving rsp.  When we restore rsp and `ret`,
; execution continues right after the call site in the "from" process.

bits 64
section .text

global context_switch
context_switch:
    ; ── Save current (from) context ──────────────────────────────────────
    ; Push callee-saved regs onto the current kernel stack.
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    ; Save the stack pointer into from->rsp  (rdi = from)
    ; cpu_ctx_t layout: rsp at offset 0, then rbx, rbp, r12, r13, r14, r15
    ; We already pushed rbx..r15 so rsp now points at r15 on the stack.
    ; Save it into from->rsp.
    mov     [rdi + 0], rsp      ; from->rsp = rsp

    ; Also save the individual registers into the struct (for debugging /
    ; future use by a debugger that inspects the PCB directly).
    mov     [rdi + 8],  rbx
    mov     [rdi + 16], rbp
    mov     [rdi + 24], r12
    mov     [rdi + 32], r13
    mov     [rdi + 40], r14
    mov     [rdi + 48], r15

    ; ── Switch address space ──────────────────────────────────────────────
    ; Write new_pml4 (rdx) to CR3.  This flushes the TLB.
    ; Do this BEFORE switching rsp so we are still on a mapped stack.
    test    rdx, rdx
    jz      .skip_cr3           ; new_pml4 == 0 → same address space, skip
    mov     cr3, rdx
.skip_cr3:

    ; ── Restore next (to) context ─────────────────────────────────────────
    ; Switch to the new process's kernel stack (rsi = to).
    mov     rsp, [rsi + 0]      ; rsp = to->rsp

    ; Pop callee-saved registers from the new stack.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx

    ; `ret` pops the return address that was pushed when THIS process
    ; last called context_switch — jumping back into that process's
    ; execution right after its context_switch call site.
    ;
    ; For a brand-new process (first time it runs), the return address is
    ; proc_trampoline (placed by process_create).  r12 was also set by
    ; process_create to the actual entry function — proc_trampoline uses it.
    ret

; ── proc_trampoline ───────────────────────────────────────────────────────
; Entry point for ALL new kernel-mode processes on their very first run.
;
; WHY this exists:
;   context_switch enters a new process via `ret`, not `iretq`.
;   iretq is what normally restores RFLAGS (including IF=1).
;   Without it, the new process runs with IF=0 — interrupts disabled.
;   The PIT can fire but the CPU ignores it, so the scheduler never preempts
;   the process and no other process ever gets to run.
;
; HOW it works:
;   process_create puts proc_trampoline as the fake return address so
;   context_switch's `ret` lands here.  The actual entry function is passed
;   in r12 (a callee-saved register that survived the pops in context_switch).
;
global proc_trampoline
proc_trampoline:
    sti         ; re-enable interrupts — PIT can now preempt this process
    call r12    ; call the real entry function (set by process_create)
    ; If the entry function returns, mark dead and yield.
.dead:
    cli
    hlt
    jmp .dead

; ── serial_hex64: print rax as 16 hex digits to COM1 (0x3F8) ─────────────
; Clobbers: rax, rbx, rcx, rdx.
serial_hex64:
    mov  rcx, 16
.loop:
    rol  rax, 4
    mov  rbx, rax
    and  rbx, 0xF
    add  rbx, '0'
    cmp  rbx, '9'
    jle  .ok
    add  rbx, 7
.ok:
    ; wait for TX ready
.wait:
    mov  rdx, 0x3F8 + 5    ; LSR
    in   al, dx
    test al, 0x20
    jz   .wait
    mov  rdx, 0x3F8
    mov  al, bl
    out  dx, al
    loop .loop
    ; print newline
    mov  rdx, 0x3F8 + 5
.waitnl:
    in   al, dx
    test al, 0x20
    jz   .waitnl
    mov  rdx, 0x3F8
    mov  al, 0x0A
    out  dx, al
    ret

; ── user_trampoline ───────────────────────────────────────────────────────
; Entry point for NEW USER-MODE processes on their very first run.
;
; On entry (from context_switch via ret):
;   r12 = user entry RIP  (0x400000)
;   r13 = user stack RSP  (VMM_USER_STACK_TOP)
;   interrupts: whatever state the previous task had (may be on)
;
; Build an iretq frame and execute iretq to enter ring 3.
; iretq pops: [rsp] = RIP, [rsp+8] = CS, [rsp+16] = RFLAGS,
;             [rsp+24] = RSP, [rsp+32] = SS.
global user_trampoline
user_trampoline:
    cli                     ; disable interrupts while building frame

    ; Push iretq frame (push in reverse order: SS first, RIP last).
    mov  rax, 0x23          ; USER_SS  (user data64, DPL=3 → 0x20|3)
    push rax
    push r13                ; user RSP
    pushfq
    or   qword [rsp], (1<<9); set IF=1 in the frame so user space has interrupts
    mov  rax, 0x2B          ; USER_CS  (user code64, DPL=3 → 0x28|3)
    push rax
    push r12                ; user RIP

    iretq
