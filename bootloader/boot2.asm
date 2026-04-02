; boot2.asm (stage2)
; org 0x8000, 16-bit real mode entry
; - Loads kernel to 0x20000 via INT 13h AH=42h
; - Collects E820 map via INT 15h EAX=E820 (writes into bootinfo)
; - Collects VBE info via INT 10h 4F01/4F02 (writes into bootinfo)
; - Enters protected mode then long mode, passes RDI=&bootinfo, jumps 0x20000

org 0x8000
bits 16

E820_MAX        equ 128
E820_ENTRY_SIZE equ 24
VBE_MODE        equ 0x118

start:
  xor ax, ax
  mov ds, ax
  mov es, ax

  cli
  mov ss, ax
  mov sp, 0x7C00
  sti

  mov [boot_drive], dl

  ; Write unmistakable signature into bootinfo so you can verify bytes in GDB
  mov dword [bootinfo + 0],  0x11112222
  mov dword [bootinfo + 4],  0x33334444
  mov dword [bootinfo + 8],  0x55556666
  mov dword [bootinfo + 12], 0x77778888

  ; --------------------------
  ; Load kernel to 0x20000
  ; --------------------------
  mov ah, 0x42
  mov dl, [boot_drive]
  mov si, dap_kernel
  int 0x13
  jc disk_error

  ; --------------------------
  ; Gather BIOS info
  ; --------------------------
  call get_e820_map
  ;call get_vbe

enter_protected_mode:
  cli
  in  al, 0x92
  or  al, 0x02
  and al, 0xFE
  out 0x92, al

  lgdt [gdt_desc]

  mov eax, cr0
  or  eax, 1
  mov cr0, eax

  jmp 0x08:protected_entry

; ============================================================
; E820 memory map (writes to ES:DI) + debug capture
; ============================================================
get_e820_map:
    xor ax, ax
    mov es, ax
    mov ds, ax
    xor ebx, ebx
    xor bp, bp
    mov di, e820_map

.e820_loop:
    cli
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24             ; Standard request size
    
    ; Ensure DI is updated correctly without IMUL inside the loop
    ; DI should point to e820_map + (bp * 24)
    
    int 0x15
    
    jc .error_or_done       ; CF set is standard "end of list" or error
    cmp eax, 0x534D4150     ; BIOS must return 'SMAP' in EAX
    jne .error_or_done

    ; Success: entry written to ES:DI
    inc bp
    add di, 24              ; Move buffer pointer for next entry
    
    test ebx, ebx           ; If EBX is 0, list is finished
    jz .done
    
    cmp bp, E820_MAX
    jb .e820_loop
    
.done:
    mov [e820_count], bp
    ret
    sti
.error_or_done:
    ; If bp > 0, we likely just hit the end of the list (normal)
    ; If bp == 0, the BIOS call failed immediately
    jmp .done
; ============================================================
; VBE info + set mode (writes to ES:DI) + debug capture
; ============================================================
get_vbe:
  xor ax, ax
  mov ds, ax
  mov es, ax

  ; 4F01h: get mode info
  mov ax, 0x4F01
  mov cx, VBE_MODE
  mov di, vbe_mode_info

  int 0x10

  mov [vbe_dbg_ax_4f01], ax
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

  ; 4F02h: set mode (with LFB)
  mov ax, 0x4F02
  mov bx, VBE_MODE
  or  bx, 0x4000
  int 0x10
  mov [vbe_dbg_ax_4f02], ax
  cmp ax, 0x004F
  jne .fail

  mov word [vbe_mode], VBE_MODE
  ret

.fail:
  mov word [vbe_mode], 0
  mov word [vbe_w], 0
  mov word [vbe_h], 0
  mov word [vbe_pitch], 0
  mov byte [vbe_bpp], 0
  mov dword [vbe_fb], 0
  ret

; ============================================================
; 32-bit protected mode
; ============================================================
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
  mov eax, cr0
  and eax, 0x7FFFFFFF
  mov cr0, eax

  mov eax, cr4
  or  eax, 0x00000020
  mov cr4, eax

  mov eax, pml4
  mov cr3, eax

  mov ecx, 0xC0000080
  rdmsr
  or  eax, 1 << 8
  wrmsr

  mov eax, cr0
  or  eax, 0x80000000
  mov cr0, eax

  jmp 0x18:long_entry

; ============================================================
; 64-bit long mode entry
; ============================================================
bits 64
long_entry:
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov ss, ax

  lea rdi, [rel bootinfo]
  mov rax, 0x00020000
  jmp rax

disk_error:
  hlt
  jmp disk_error

; ============================================================
; Data (low memory)
; ============================================================
bits 16
boot_drive db 0

; ---- bootinfo block (kernel reads this via RDI) ----
; Offsets (bytes) from bootinfo:
;  0..15   signature (4 dwords)
;  16..   e820 debug + e820_count + e820_map + vbe fields...
bootinfo:
  dd 0,0,0,0                      ; 0..15 signature (filled at runtime)

e820_dbg_flags_before dw 0         ; 16
e820_dbg_es           dw 0         ; 18
e820_dbg_di           dw 0         ; 20

e820_dbg_eax_before   dd 0         ; 22
e820_dbg_ebx_before   dd 0         ; 26
e820_dbg_ecx_before   dd 0         ; 30
e820_dbg_edx_before   dd 0         ; 34

e820_dbg_flags_after  dw 0         ; 38
e820_dbg_cf_after     db 0         ; 40
  db 0                             ; 41 pad

e820_dbg_eax_after    dd 0         ; 42
e820_dbg_ebx_after    dd 0         ; 46
e820_dbg_ecx_after    dd 0         ; 50
e820_dbg_edx_after    dd 0         ; 54

e820_count            dw 0         ; 58
e820_map              times (E820_MAX * E820_ENTRY_SIZE) db 0

vbe_dbg_ax_4f01        dw 0
vbe_dbg_ax_4f02        dw 0

vbe_mode               dw 0
vbe_w                  dw 0
vbe_h                  dw 0
vbe_pitch              dw 0
vbe_bpp                db 0
align 4
vbe_fb                 dd 0
vbe_mode_info          times 256 db 0

kernel_phys_base        dq 0
phys_ceiling            dq 0
hhdm_offset             dq 0
pml4_phys               dq 0

; ---- INT 13h extended read packet ----
dap_kernel:
  db 16
  db 0
  dw 16
  dw 0
  dw 0x2000
  dq 33

; ============================================================
; GDT
; ============================================================
gdt_desc:
  dw gdt_end - gdt - 1
  dd gdt

gdt:
  dq 0
  dq 0x00CF9A000000FFFF
  dq 0x00CF92000000FFFF
  dq 0x00AF9A000000FFFF
gdt_end:

; ============================================================
; Paging (identity map low memory using 2 MiB pages)
; ============================================================
align 4096
pml4:
  dq pdpt + 0b11
  times 511 dq 0

align 4096
pdpt:
  dq pd + 0b11
  times 511 dq 0

align 4096
pd:
  dq 0x00000083
  dq 0x00200083
  dq 0x00400083
  dq 0x00600083
  dq 0x00800083
  dq 0x00A00083
  dq 0x00C00083
  dq 0x00E00083
  dq 0x01000083
  dq 0x01200083
  dq 0x01400083
  dq 0x01600083
  dq 0x01800083
  dq 0x01A00083
  dq 0x01C00083
  dq 0x01E00083
  dq 0x02000083
  dq 0x02200083
  dq 0x02400083
  dq 0x02600083
  dq 0x02800083
  dq 0x02A00083
  dq 0x02C00083
  dq 0x02E00083
  dq 0x03000083
  dq 0x03200083
  dq 0x03400083
  dq 0x03600083
  dq 0x03800083
  dq 0x03A00083
  dq 0x03C00083
  dq 0x03E00083
  dq 0x04000083
  dq 0x04200083
  dq 0x04400083
  dq 0x04600083
  dq 0x04800083
  dq 0x04A00083
  dq 0x04C00083
  dq 0x04E00083
  dq 0x05000083
  dq 0x05200083
  dq 0x05400083
  dq 0x05600083
  dq 0x05800083
  dq 0x05A00083
  dq 0x05C00083
  dq 0x05E00083
  dq 0x06000083
  dq 0x06200083
  dq 0x06400083
  dq 0x06600083
  dq 0x06800083
  dq 0x06A00083
  dq 0x06C00083
  dq 0x06E00083
  dq 0x07000083
  dq 0x07200083
  dq 0x07400083
  dq 0x07600083
  dq 0x07800083
  dq 0x07A00083
  dq 0x07C00083
  dq 0x07E00083
  times (512-64) dq 0
