bits 64

section .text
  global _start
  extern kmain

_start:
  cli
  cld
  mov [bootinfo_ptr], rdi

  mov rax, 0x00700000 ; Force 64-bit immediate load
  mov rsp, rax        ; Move to RSP
  call kmain

.halt:
  hlt
  jmp .halt

section .data
  global bootinfo_ptr
bootinfo_ptr dq 0

section .note.GNU-stack noalloc noexec nowrite progbits
