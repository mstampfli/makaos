bits 64
default rel

; c handlers 

extern isr0_divide_error
extern isr1_debug
extern isr2_nmi

extern isr5_bound
extern isr6_invalid_opcode
extern isr7_device_na

extern isr8_double_fault
extern isr9_coprocessor_overrun

extern isr10_invalid_tss
extern isr11_seg_np
extern isr12_stack_fault
extern isr13_gp
extern isr14_page_fault

extern isr16_x87_fp
extern isr17_alignment
extern isr18_machine_check
extern isr19_simd_fp
extern isr20_virtualization
extern isr21_control_protection

global isr_common_entry

; small helpers (macros because if you'd call then youd use stack)

%macro PUSH_GPRS 0
  push r15
  push r14
  push r13
  push r12
  push r11
  push r10
  push r9
  push r8
  push rdi
  push rsi
  push rbp
  push rdx
  push rcx
  push rbx
  push rax
%endmacro

%macro POP_GPRS 0
  pop rax
  pop rbx
  pop rcx
  pop rdx
  pop rbp
  pop rsi
  pop rdi
  pop r8
  pop r9
  pop r10
  pop r11
  pop r12
  pop r13
  pop r14
  pop r15
%endmacro

; common entry
; Stack on entry MUST be:
;   [has_ec: 0 or 1][c_handler_ptr][ec][ip][cs][flags][sp][ss]
; has_ec and c_handler_ptr are pushed by the ISR_NOEC/ISR_EC macros below,
; AFTER saving all GPRs so that rdi/rsi are not clobbered before PUSH_GPRS.
isr_common_entry:
  PUSH_GPRS

  ; After PUSH_GPRS the stack is the exact trap_frame_t layout (idt.h),
  ; low → high addresses:
  ;   [rax rbx rcx rdx rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15]  ; 15 GPRs
  ;   [has_ec] [handler] [ec] [ip] [cs] [flags] [sp] [ss]
  ;    rsp+0..112          rsp+120  rsp+128 rsp+136 ...
  ; We pass the C handler a pointer to the REAL frame's `ip` field (an
  ; interrupt_frame_t*), NOT a copy — so any edit the handler makes to the
  ; saved GPRs (via TRAP_FROM_IFRAME) or to ip/sp/flags takes effect on the
  ; POP_GPRS + iretq below.  That is how a synchronous fault delivers a
  ; signal to a user handler (signal_deliver_fault redirects ip/sp + rdi/rsi/rdx).
  lea rbx, [rsp + 15*8]   ; rbx → &has_ec (rbx is callee-saved: survives the call)

  ; CPU frame fields: rbx+0(has_ec) rbx+8(handler) rbx+16(ec)
  ;   rbx+24(ip) rbx+32(cs) rbx+40(flags) rbx+48(sp) rbx+56(ss)
  mov rcx, [rbx + 8]     ; rcx = c handler address
  mov rdx, [rbx + 0]     ; rdx = has_ec flag
  lea rdi, [rbx + 24]    ; arg0 = &ip = interrupt_frame_t* into the REAL frame

  sub rsp, 8             ; 16-byte-align rsp for the SysV call (was ≡8 mod 16)

  test rdx, rdx
  jz .call_no_ec

  mov rsi, [rbx + 16]     ; arg1 = ec
  call rcx
  jmp .after_call

.call_no_ec:
  call rcx

.after_call:
  add rsp, 8             ; undo the alignment pad → rsp back at GPR base

  POP_GPRS               ; restore (possibly handler-modified) GPRs

  add rsp, 8*3           ; drop has_ec + handler + ec
  iretq                  ; return via (possibly handler-modified) ip/cs/flags/sp/ss

; IDT entry generators
; Push has_ec and handler BEFORE touching rdi/rsi so PUSH_GPRS saves originals.
%macro ISR_NOEC 2
  global isr%1_entry
  isr%1_entry:
    push qword 0          ; ec = 0 (placeholder)
    push qword %2         ; c handler address
    push qword 0          ; has_ec = 0
    jmp isr_common_entry
%endmacro

%macro ISR_EC 2
  global isr%1_entry
  isr%1_entry:
    ; ec already on stack (pushed by CPU)
    push qword %2         ; c handler address
    push qword 1          ; has_ec = 1
    jmp isr_common_entry
%endmacro

; vectors 0-2 + 5-14 + 16-21
; Error-code vectors: 8,10,11,12,13,14,17,21
ISR_NOEC 0,  isr0_divide_error
ISR_NOEC 1,  isr1_debug
ISR_NOEC 2,  isr2_nmi

ISR_NOEC 5,  isr5_bound
ISR_NOEC 6,  isr6_invalid_opcode
ISR_NOEC 7,  isr7_device_na

ISR_EC   8,  isr8_double_fault

ISR_NOEC 9,  isr9_coprocessor_overrun

ISR_EC   10, isr10_invalid_tss
ISR_EC   11, isr11_seg_np
ISR_EC   12, isr12_stack_fault
ISR_EC   13, isr13_gp
ISR_EC   14, isr14_page_fault

ISR_NOEC 16, isr16_x87_fp
ISR_EC   17, isr17_alignment
ISR_NOEC 18, isr18_machine_check
ISR_NOEC 19, isr19_simd_fp
ISR_NOEC 20, isr20_virtualization
ISR_EC   21, isr21_control_protection

