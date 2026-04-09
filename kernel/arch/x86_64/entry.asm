bits 64

STACK_SIZE equ 32768  ; 32 KiB initial stack

section .text
  global _start
  extern kmain

_start:
  cli
  cld
  mov [bootinfo_ptr], rdi

  ; Use a stack in the kernel image (.data) — not in .bss so BSS clear is safe
  lea rsp, [init_stack_top]
  call kmain

.halt:
  hlt
  jmp .halt

section .data
  global bootinfo_ptr
bootinfo_ptr dq 0

  ; Boot stack lives in .data (already mapped, not zeroed by BSS clear)
  align 16
  times STACK_SIZE db 0
  init_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
