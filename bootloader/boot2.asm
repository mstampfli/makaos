org 0x8000 ; Bootloader stage2 starts at memory address 0x8000
bits 16

E820_MAX equ 64
E820_ENTRY_SIZE equ 24

VBE_MODE equ 0x118

start:
  xor ax, ax
  mov ds, ax
  mov es, ax

  cli
  mov ss, ax
  mov sp, 0x9800
  sti 

  mov [boot_drive], dl
  mov ah, 0x42
  mov dl, [boot_drive]
  mov si, dap_kernel 
  int 0x13
  jc disk_error

  call get_e820_map
  call get_vbe
  
enter_protected_mode:
  cli
  in al, 0x92
  or al, 0x02 ; = 00000010 -> activate A20 via port 0x92 by setting bit 1 to 1 (disables address wrapping at addresses higher than 20 bit (1 Mib))
  and al, 0xFE; = 11111110 -> force clear bit 0
  out 0x92, al
  
  lgdt [gdt_desc]

  mov eax, cr0
  or eax, 1
  mov cr0, eax

  jmp 0x08:protected_entry

get_e820_map:
  xor ebx, ebx
  xor bp, bp

.e820_loop:
  mov eax, 0xE820
  mov edx, 0x534D4150
  mov ecx, 24

  mov di, e820_map
  mov ax, bp
  imul ax, E820_ENTRY_SIZE
  add di, ax

  int 0x15
  jc .done

  inc bp
  cmp bp, E820_MAX
  jae .done

  test ebx, ebx
  jne .e820_loop

.done:
  mov [e820_count], bp
  ret

get_vbe:
  mov ax, 0x4F01
  mov cx, VBE_MODE
  mov di, vbe_mode_info
  int 0x10
  cmp ax, 0x004F
  jne .fail

  mov ax, [vbe_mode_info + 0x10]
  mov [vbe_pitch], ax

  mov ax, [vbe_mode_info + 0x12]
  mov [vbe_w], ax

  mov ax, [vbe_mode_info + 0x14]
  mov [vbe_h], ax

  mov al, [vbe_mode_info + 0x19]
  mov [vbe_bpp], al

  mov eax, [vbe_mode_info + 0x28]
  mov [vbe_fb], eax

  mov ax, 0x4F02
  mov bx, VBE_MODE
  or bx, 0x4000
  int 0x10
  cmp ax, 0x004F
  jne .fail

  mov word [vbe_mode], VBE_MODE
  ret

.fail:
  mov dword [vbe_fb], 0
  mov word  [vbe_pitch], 0
  mov word  [vbe_w], 0
  mov word  [vbe_h], 0
  mov byte  [vbe_bpp], 0
  mov word  [vbe_mode], 0
  ret

bits 32
protected_entry:
  cld
  cli
  mov ax, 0x10 
  mov ds, ax
  mov es, ax
  mov ss, ax

  mov esp, 0x00009800

enter_long_mode:
;cr's = 32 bit
;step 1: force disable paging -> cr0.pg (bit 31)
  mov eax, cr0
  and eax, 0x7FFFFFFF
  mov cr0, eax

;step 2: enable page addresses to go over 32 bits -> cr4.pae (bit 5)
  mov eax, cr4
  or eax, 0x00000020
  mov cr4, eax

;step 3: load cr3 with physical base address of pml4 (and flags)
  mov eax, pml4
  mov cr3, eax

;step 4: enable long mode (via msr selector 0xC0000080)
  mov ecx, 0xC0000080
  rdmsr
  or eax, 1 << 8
  wrmsr

;step 5: enable paging by setting cr0.pg
  mov eax, cr0
  or eax, 0x80000000
  mov cr0, eax

  jmp 0x18:long_entry

bits 64
long_entry:
  mov ax, 0x10 
  mov ds, ax
  mov es, ax
  mov ss, ax
 
  lea rdi, [rel bootinfo]         ; ptr for kernel use

  mov rax, 0x00020000
  jmp rax
  
disk_error:
  hlt
  jmp disk_error

boot_drive db 0

bootinfo:
e820_count dw 0
e820_map times (E820_MAX * E820_ENTRY_SIZE) db 0

vbe_mode dw 0
vbe_w    dw 0
vbe_h    dw 0
vbe_pitch dw 0
vbe_bpp  db 0
align 4
vbe_fb   dd 0

vbe_mode_info times 256 db 0

dap_kernel:
  db 16
  db 0;
  dw 16;
  dw 0;
  dw 0x2000;
  dq 33;
 
gdt_desc:
  dw gdt_end - gdt - 1 ;limit (bytes - 1)
  dd gdt ;base

;base = 0x00000000 (32bits) 
;bits 16-39 & 56-63

;limit (we want max) = 0xFFFF (20bits)
;bits 0-15 & 48-51

;access byte = 10011010 -> 0x9A (code); 10010010 -> 0x92 (data)
;  7: P; present = 1
;  6-5: DPL; privilege level = 0 for Kernel
;  4: S; 1 = code/data 0 = systems (ignore)
;  3: E; 1 = code/executable, 0 = data
;  2: DC; Direction (data) 0 = grow downwards / Conforming (code) from what privilege can i call? 1 = anywhere; 0 = same privilege
;  1: RW; Writable (data) / Readable (code)
;  0: A; Accessed (CPU sets this)
;bits 40-47

;flags: 1100 -> 0xC
;  7: G; granularity, 1 = 4KiB units
;  6: D; default operand size, 1 = 32-bit
;  5: L; Long mode, 1 = 64 bit ONLY
;  4: AVL; unused
;bits 52-55
gdt:
  dq 0
  dq 0x00CF9A000000FFFF; code 32bit
  dq 0x00CF92000000FFFF; data
  dq 0x00AF9A000000FFFF; code 64bit 
gdt_end:

align 4096
pml4:
  dq pdpt + 0b11       ; Address of PDPT, Present, RW
  times 511 dq 0

align 4096
pdpt:
    dq pd + 0b11         ; Address of PD, Present, RW
    times 511 dq 0

align 4096
pd:
  ;1 P: present = 1
  ;1 R/W: write = 1
  ;0 U/S: only kernel access
  ;0 PWT
  ;0 PCD
  ;0 A: accessed
  ;0 D: dirty
  ;1 PS: pagesize = 1 otherwise it references a page table
  ;0 G: global
  ;3 * 0 ignored
  ;0 PAT
  ;8 * 0 reserved
  ;address 0x0
  ;pad to 64
  dq 0x00000083
  dq 0x00200083                  ; 2 MiB  
  dq 0x00400083                  ; 4 MiB 
  dq 0x00600083                  ; 6 MiB
  dq 0x00800083                  ; 8 MiB
  dq 0x00A00083                  ; 10 MiB
  dq 0x00C00083                  ; 12 MiB
  dq 0x00E00083                  ; 14 MiB
  times (512-8) dq 0
