bits 64
default rel

; ── IRQ handlers ─────────────────────────────────────────────────────────
;
; Hardware IRQs differ from CPU exceptions:
;   • The CPU does NOT push an error code.
;   • We must send EOI (End-of-Interrupt) to the PIC ourselves.
;
; EOI timing: we send it EARLY, before calling the C handler.
; Reason: if the C handler triggers a context switch into a NEW process
; (one whose stack has a fake initial frame, not an interrupt frame), that
; process will never return through this stub, so the EOI would never be
; sent.  With early EOI the PIT re-arms itself immediately; since we are
; in an interrupt gate (CPU cleared IF on entry), no spurious re-entry is
; possible until iretq restores RFLAGS.
;
; Stack alignment after PUSH_GPRS:
;   Interrupted process had RSP 16-byte aligned (ABI).
;   CPU pushes 5 × 8 = 40 bytes  → RSP % 16 = 8  (misaligned).
;   PUSH_GPRS pushes 15 × 8 = 120 bytes → RSP % 16 = 0  (aligned).
;   `call` then pushes the return address (8 bytes) → inside the C
;   function RSP % 16 = 8, which is correct per SysV AMD64 ABI.

extern timer_irq_handler
extern keyboard_irq_handler
extern mouse_irq_handler
extern ahci_irq_handler
extern hda_irq_handler
extern g_ahci_irq      ; uint8_t — IRQ line (0-15), set by ahci_init
extern g_hda_irq       ; uint8_t — IRQ line (0-15), set by hda_init

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

section .text

; ── IRQ0: PIT timer (vector 0x20) ────────────────────────────────────────
global irq0_entry
irq0_entry:
    PUSH_GPRS

    ; Early EOI to master PIC (IRQ0 is on the master).
    ; Port 0x20 = PIC1_CMD, byte 0x20 = PIC_EOI.
    mov al, 0x20
    out 0x20, al

    call timer_irq_handler

    POP_GPRS
    iretq

; ── IRQ1: PS/2 keyboard (vector 0x21) ────────────────────────────────────
global irq1_entry
irq1_entry:
    PUSH_GPRS

    ; Early EOI to master PIC (IRQ1 is on the master).
    mov al, 0x20
    out 0x20, al

    call keyboard_irq_handler

    POP_GPRS
    iretq

; ── IRQ12: PS/2 mouse (vector 0x2C) ─────────────────────────────────────
; IRQ12 is on the slave PIC (line 4 of slave = IRQ8+4=12).
; Must send EOI to both slave (0xA0) and master (0x20).
global irq12_entry
irq12_entry:
    PUSH_GPRS

    mov al, 0x20
    out 0xA0, al    ; slave EOI
    out 0x20, al    ; master EOI (cascade)

    call mouse_irq_handler

    POP_GPRS
    iretq

; ── AHCI: generic stub, IRQ line read from g_ahci_irq ────────────────────
; Slave IRQs (8-15): EOI to slave PIC (0xA0) then master (0x20).
; Master IRQs (0-7): EOI to master only.
global ahci_irq_entry
ahci_irq_entry:
    PUSH_GPRS

    mov al, 0x20
    movzx ecx, byte [rel g_ahci_irq]
    cmp cl, 8
    jl .master_only
    out 0xA0, al    ; slave EOI
.master_only:
    out 0x20, al    ; master EOI

    call ahci_irq_handler

    POP_GPRS
    iretq

; ── HDA audio: IRQ line from g_hda_irq (PCI-routed, master or slave) ────
global hda_irq_entry
hda_irq_entry:
    PUSH_GPRS

    mov al, 0x20
    movzx ecx, byte [rel g_hda_irq]
    cmp cl, 8
    jl .master_only
    out 0xA0, al    ; slave EOI
.master_only:
    out 0x20, al    ; master EOI

    call hda_irq_handler

    POP_GPRS
    iretq

; ── Spurious IRQ stubs ────────────────────────────────────────────────────
; IRQ7  (vec 0x27): spurious from master PIC — no EOI at all.
; IRQ15 (vec 0x2F): spurious from slave PIC  — EOI master only (cascade).
global spurious_irq7_entry
spurious_irq7_entry:
    iretq

global spurious_irq15_entry
spurious_irq15_entry:
    ; Master saw a real IRQ2 (cascade) so it needs an EOI; slave did not.
    mov al, 0x20
    out 0x20, al
    iretq
