; setjmp / longjmp for x86-64 System V ABI
;
; jmp_buf layout (uint64_t[9]):
;   [0]  rbx
;   [1]  rbp
;   [2]  r12
;   [3]  r13
;   [4]  r14
;   [5]  r15
;   [6]  rsp   (caller's RSP at the point of the setjmp call)
;   [7]  rip   (return address = where setjmp was called from)
;   [8]  mxcsr (SSE control/status register)

bits 64
section .text

global setjmp
global longjmp
global _setjmp
global _longjmp

; int setjmp(jmp_buf env)   — rdi = env
; Returns 0.  On longjmp, resumes here returning the longjmp val.
setjmp:
_setjmp:
    ; Save callee-saved integer registers.
    mov  [rdi +  0], rbx
    mov  [rdi +  8], rbp
    mov  [rdi + 16], r12
    mov  [rdi + 24], r13
    mov  [rdi + 32], r14
    mov  [rdi + 40], r15
    ; Save caller's RSP (RSP before the call to setjmp = RSP + 8 because
    ; `call setjmp` already pushed the return address).
    lea  rax, [rsp + 8]
    mov  [rdi + 48], rax
    ; Save return address (= where to resume when longjmp is called).
    mov  rax, [rsp]
    mov  [rdi + 56], rax
    ; Save MXCSR.
    stmxcsr [rdi + 64]
    ; Return 0.
    xor  eax, eax
    ret

; void longjmp(jmp_buf env, int val)   — rdi = env, rsi = val
; Resumes execution at the setjmp site, returning val (or 1 if val == 0).
longjmp:
_longjmp:
    ; Compute return value in rax: val if val != 0, else 1.
    mov  eax, esi
    test eax, eax
    jnz  .nonzero
    mov  eax, 1
.nonzero:
    ; Restore MXCSR before touching any register state.
    ldmxcsr [rdi + 64]
    ; Restore callee-saved registers.
    ; Use r11 as a scratch for the saved RIP (r11 is caller-clobbered so safe).
    mov  r11, [rdi + 56]  ; saved RIP
    mov  rbx, [rdi +  0]
    mov  rbp, [rdi +  8]
    mov  r12, [rdi + 16]
    mov  r13, [rdi + 24]
    mov  r14, [rdi + 32]
    mov  r15, [rdi + 40]
    ; Switch stack.
    mov  rsp, [rdi + 48]
    ; Jump to saved RIP.  We cannot `ret` here because we have not pushed
    ; anything; instead push then ret, which is the standard idiom.
    push r11
    ret
