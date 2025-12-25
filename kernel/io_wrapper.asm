bits 64

global outb
outb:
    ; RDI = port, RSI = val
    mov dx, di
    mov al, sil
    out dx, al
    ret

global inb
inb:
    ; RDI = port, returns AL
    mov dx, di
    in  al, dx
    movzx eax, al
    ret

global outw
outw:
    ; RDI = port, RSI = val
    mov dx, di
    mov ax, si
    out dx, ax
    ret

global inw
inw:
    ; RDI = port, returns AX
    mov dx, di
    in  ax, dx
  movzx eax, ax
  ret

global insw
insw:
  ; RDI = port, RSI = dst, RDX = count_words
  mov dx, di
  mov rdi, rsi      ; rep insw uses RDI as destination
  mov rcx, rdx
  cld
  rep insw
  ret

global outsw
outsw:
    ; RDI = port, RSI = src, RDX = count_words
    mov dx, di
    mov rcx, rdx
    cld
    rep outsw         ; uses RSI as source
    ret

; SysV caller-saved regs:
; RAX RCX RDX RSI RDI R8 R9 R10 R11
;
; no_caller_saved_registers requires callees to preserve caller-saved regs.
; Only push/pop the ones each function actually clobbers.

global outb_irq
outb_irq:
    ; RDI = port, RSI = val
    ; clobbers: RAX, RDX
    push rax
    push rdx

    mov dx, di
    mov al, sil
    out dx, al

    pop rdx
    pop rax
    ret

global inb_irq
inb_irq:
    ; RDI = port, returns AL (zero-extended in EAX)
    ; clobbers: RDX, RAX
    push rdx

    mov dx, di
    in  al, dx
    movzx eax, al

    pop rdx
    ret

global outw_irq
outw_irq:
    ; RDI = port, RSI = val
    ; clobbers: RAX, RDX
    push rax
    push rdx

    mov dx, di
    mov ax, si
    out dx, ax

    pop rdx
    pop rax
    ret

global inw_irq
inw_irq:
    ; RDI = port, returns AX (zero-extended in EAX)
    ; clobbers: RDX, RAX
    push rdx

    mov dx, di
    in  ax, dx
    movzx eax, ax

    pop rdx
    ret

global insw_irq
insw_irq:
    ; RDI = port, RSI = dst, RDX = count_words
    ; rep insw clobbers: RCX, RDI
    ; setup clobbers: RDX
    ; also uses RSI as input (we read it), but we don't need to preserve RSI unless we change it
    push rcx
    push rdx
    push rdi

    mov dx, di
    mov rdi, rsi
    mov rcx, rdx
    cld
    rep insw

    pop rdi
    pop rdx
    pop rcx
    ret

global outsw_irq
outsw_irq:
    ; RDI = port, RSI = src, RDX = count_words
    ; rep outsw clobbers: RCX, RSI
    ; setup clobbers: RDX
    push rcx
    push rdx
    push rsi

    mov dx, di
    mov rcx, rdx
    cld
    rep outsw

    pop rsi
    pop rdx
    pop rcx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
