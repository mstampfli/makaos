bits 64

_start:
  cli
  mov rsp, 0x00020000
  extern kmain
  jmp kmain



