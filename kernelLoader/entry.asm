bits 64

section .text
  global _start
  extern kmain

; boot_info_t is packed, E820_MAX = 128, e820_entry_t assumed 24 bytes.
; Offsets:
;   pml4_phys        @ 3430 (0xD66)
%define OFF_PML4_PHYS 3430

%define KERNEL_HH_BASE 0xFFFFFFFF80000000

_start:
  cli
  cld

  ; Save boot_info_t* across the C call (RDI is caller-saved)
  mov rbx, rdi

  ; Stack in identity-mapped low memory
  mov rsp, 0x00020000
  sub rsp, 8

  ; kmain(boot_info_t* info)
  mov rdi, rbx
  call kmain

  add rsp, 8
  
  ; Restore boot_info_t*
  mov rdi, rbx

  ; Load CR3 from bootinfo->pml4_phys (page tables built by kmain)
  mov rax, [rdi + OFF_PML4_PHYS]
  mov cr3, rax

  ;jmp .halt
  ; Kernel is mapped to EXACTLY this virtual base (2MiB mapping).
  mov rax, KERNEL_HH_BASE
  jmp rax

.halt:
  hlt
  jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
