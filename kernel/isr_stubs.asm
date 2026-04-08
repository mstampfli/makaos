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

  ; After PUSH_GPRS the stack looks like (low → high addresses):
  ;   [15 GPRs × 8] [has_ec] [handler] [ec] [ip] [cs] [flags] [sp] [ss]
  ;    rsp+0..112    rsp+120   rsp+128  rsp+136  ...
  ; We use rbx as a scratch pointer into the CPU frame area.
  lea rbx, [rsp + 15*8]   ; rbx → &has_ec

  ; Allocate interrupt_frame_t (5 qwords)
  sub rsp, 5*8

  ; CPU frame fields are at rbx+0(has_ec) rbx+8(handler) rbx+16(ec)
  ;   rbx+24(ip) rbx+32(cs) rbx+40(flags) rbx+48(sp) rbx+56(ss)

  ; frame.ip
  mov rax, [rbx + 24]
  mov [rsp + 0], rax

  ; frame.cs
  mov rax, [rbx + 32]
  mov [rsp + 8], rax

  ; frame.flags
  mov rax, [rbx + 40]
  mov [rsp + 16], rax

  ; frame.sp
  mov rax, [rbx + 48]
  mov [rsp + 24], rax

  ; frame.ss
  mov rax, [rbx + 56]
  mov [rsp + 32], rax

  ; load handler pointer and has_ec flag
  mov rcx, [rbx + 8]     ; rcx = c handler address
  mov rdx, [rbx + 0]     ; rdx = has_ec flag

  lea rdi, [rsp]          ; arg0 = &frame

  test rdx, rdx
  jz .call_no_ec

  mov rsi, [rbx + 16]     ; arg1 = ec
  call rcx
  jmp .after_call

.call_no_ec:
  call rcx

.after_call:
  add rsp, 5*8            ; pop interrupt_frame_t

  POP_GPRS

  add rsp, 8*3            ; drop has_ec + handler + ec
  iretq

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

