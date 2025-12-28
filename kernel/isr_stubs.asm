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
; rdi = c handler address
; rsi = 0 -> handler(frame*)
;       1 -> handler(frame*, ec)
; Stack on entry MUST be:
;   [ec][ip][cs][flags][sp][ss]
isr_common_entry:
  PUSH_GPRS

  ; rbx -> start of CPU interrupt frame
  lea rbx, [rsp + 15*8]

  ; Allocate interrupt_frame_t (5 qwords)
  sub rsp, 5*8

  ; frame.ip
  mov rax, [rbx + 8]
  mov [rsp + 0], rax

  ; frame.cs
  mov rax, [rbx + 16]
  mov [rsp + 8], rax

  ; frame.flags
  mov rax, [rbx + 24]
  mov [rsp + 16], rax

  ; frame.sp
  mov rax, [rbx + 32]
  mov [rsp + 24], rax

  ; frame.ss
  mov rax, [rbx + 40]
  mov [rsp + 32], rax

  ; Call handler
  mov rcx, rdi          ; rcx = handler
  lea rdi, [rsp]        ; arg0 = &frame

  test rsi, rsi
  jz .call_no_ec

  mov rsi, [rbx + 0]    ; arg1 = ec
  call rcx
  jmp .after_call

.call_no_ec:
  call rcx

.after_call:
  add rsp, 5*8          ; pop interrupt_frame_t

  POP_GPRS

  add rsp, 8            ; drop ec
  iretq

; IDT entry generators
%macro ISR_NOEC 2
  global isr%1_entry
  isr%1_entry:
    push qword 0        ; ec = 0
    mov rdi, %2
    xor rsi, rsi
    jmp isr_common_entry
%endmacro

%macro ISR_EC 2
  global isr%1_entry
  isr%1_entry:
    mov rdi, %2
    mov rsi, 1
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

