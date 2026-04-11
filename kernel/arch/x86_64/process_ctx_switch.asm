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

; cpu_ctx_t field offsets (must match process.h):
;   rsp      = 0
;   rbx      = 8
;   rbp      = 16
;   r12      = 24
;   r13      = 32
;   r14      = 40
;   r15      = 48
;   _pad     = 56
;   fxsave   = 64   (512 bytes, 16-byte aligned because struct is aligned(16))

%define CTX_RSP      0
%define CTX_RBX      8
%define CTX_RBP      16
%define CTX_R12      24
%define CTX_R13      32
%define CTX_R14      40
%define CTX_R15      48
%define CTX_FXSAVE   64

global context_switch
context_switch:
    ; ── Save FPU/SSE state of the outgoing task ───────────────────────────
    ; FXSAVE requires 16-byte aligned memory; cpu_ctx_t is aligned(16) so
    ; fxsave_buf at offset 64 is always 16-byte aligned.
    fxsave  [rdi + CTX_FXSAVE]

    ; ── Save integer callee-saved registers ──────────────────────────────
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi + CTX_RSP], rsp
    mov     [rdi + CTX_RBX], rbx
    mov     [rdi + CTX_RBP], rbp
    mov     [rdi + CTX_R12], r12
    mov     [rdi + CTX_R13], r13
    mov     [rdi + CTX_R14], r14
    mov     [rdi + CTX_R15], r15

    ; ── Switch address space ──────────────────────────────────────────────
    test    rdx, rdx
    jz      .skip_cr3
    mov     cr3, rdx
.skip_cr3:

    ; ── Restore next (to) context ─────────────────────────────────────────
    mov     rsp, [rsi + CTX_RSP]

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx

    ; ── Restore FPU/SSE state of the incoming task ────────────────────────
    fxrstor [rsi + CTX_FXSAVE]

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

    ; Debug: print "UT:" + r12 (entry RIP) + r13 (user RSP) to COM1
    push rax
    push rbx
    push rcx
    push rdx
    ; print "UT:"
    mov  rdx, 0x3F8+5
.ut_w1: in al, dx; test al,0x20; jz .ut_w1; mov rdx,0x3F8; mov al,'U'; out dx,al
    mov  rdx, 0x3F8+5
.ut_w2: in al, dx; test al,0x20; jz .ut_w2; mov rdx,0x3F8; mov al,'T'; out dx,al
    mov  rdx, 0x3F8+5
.ut_w3: in al, dx; test al,0x20; jz .ut_w3; mov rdx,0x3F8; mov al,':'; out dx,al
    ; print r12 as hex
    mov  rax, r12
    call serial_hex64
    ; print "RSP:"
    mov  rdx, 0x3F8+5
.ut_w4: in al, dx; test al,0x20; jz .ut_w4; mov rdx,0x3F8; mov al,'R'; out dx,al
    mov  rdx, 0x3F8+5
.ut_w5: in al, dx; test al,0x20; jz .ut_w5; mov rdx,0x3F8; mov al,'S'; out dx,al
    mov  rdx, 0x3F8+5
.ut_w6: in al, dx; test al,0x20; jz .ut_w6; mov rdx,0x3F8; mov al,'P'; out dx,al
    mov  rdx, 0x3F8+5
.ut_w7: in al, dx; test al,0x20; jz .ut_w7; mov rdx,0x3F8; mov al,':'; out dx,al
    mov  rax, r13
    call serial_hex64
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

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

; ── fork_trampoline ───────────────────────────────────────────────────────
; Entry point for a newly forked child process on its very first run.
; context_switch's `ret` lands here.
;
; Stack layout on entry (pushed by task_fork, bottom-to-top):
;   [rsp+0]  user RIP   → pop into rcx for sysretq
;   [rsp+8]  user RFLAGS → pop into r11
;   [rsp+16] user RSP   → pop into rsp (switches stack to user)
;
; Child returns 0 from fork (rax = 0).
global fork_trampoline
fork_trampoline:
    xor  rax, rax        ; child returns 0 from fork
    pop  rcx             ; user RIP
    pop  r11             ; user RFLAGS
    pop  r10             ; user RSP (into r10, NOT rsp — we need kstack for iretq frame)

    ; Use iretq instead of sysretq — sysretq has a KVM/CPU bug where
    ; SS doesn't get RPL=3 OR'd in, giving SS=0x20 instead of 0x23.
    cli
    push qword 0x23      ; SS = user data64 with RPL=3
    push r10             ; RSP = user RSP
    push r11             ; RFLAGS
    push qword 0x2B      ; CS = user code64 with RPL=3
    push rcx             ; RIP

    iretq
