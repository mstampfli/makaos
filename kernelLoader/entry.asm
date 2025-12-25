bits 64

section .text
  global _start
  extern kmain

_start:
  cli
  cld
  mov [bootinfo_ptr], rdi
  mov rsp, 0x00020000
  sub rsp, 8
  call kmain
  add rsp, 8
  
  mov rdi, [bootinfo_ptr]
  mov rax, 0x00400000
  jmp rax

.halt:
  hlt
  jmp .halt

section .data
  global bootinfo_ptr
bootinfo_ptr db 0

section .note.GNU-stack noalloc noexec nowrite progbits
