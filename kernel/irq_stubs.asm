bits 64
default rel

; ── IRQ / MSI entry stubs ─────────────────────────────────────────────────
;
; All interrupt handlers use the same pattern:
;   1. PUSH_GPRS  — save all caller-saved + callee-saved GPRs
;   2. Send EOI   — to LAPIC (for LAPIC-delivered IRQs: timer, MSI, IOAPIC-routed)
;                   The LAPIC EOI register is at 0xFEE000B0 (physical).
;                   We access it via the HHDM: LAPIC_EOI_VIRT = phys + HHDM.
;                   lapic_eoi() is a C function that does the MMIO write; we
;                   call it directly from here to keep the EOI path simple.
;                   For PIC-routed ISA IRQs that now go through the IOAPIC, EOI
;                   is sent to the LAPIC only (not the PIC — the PIC is masked).
;   3. Call C handler
;   4. POP_GPRS + iretq
;
; Stack alignment:
;   Interrupted RSP is 16-byte aligned (ABI).
;   CPU pushes 5 × 8 = 40 bytes  → RSP % 16 = 8  (misaligned).
;   PUSH_GPRS pushes 15 × 8 = 120 bytes → RSP % 16 = 0  (aligned).
;   `call` pushes return address (8 bytes) → inside callee RSP % 16 = 8.
;   This satisfies the SysV AMD64 ABI.

extern timer_irq_handler
extern keyboard_irq_handler
extern mouse_irq_handler
extern ahci_irq_handler
extern hda_irq_handler
extern lapic_eoi          ; void lapic_eoi(void) — writes 0 to LAPIC EOI reg


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

; ── VEC_LAPIC_TIMER (0x31): LAPIC periodic timer ──────────────────────────
; Replaces IRQ0 (PIT).  Delivered by the LAPIC; EOI to LAPIC only.
global irq0_entry
irq0_entry:
    PUSH_GPRS
    call lapic_eoi
    call timer_irq_handler
    POP_GPRS
    iretq

; ── 0x21: PS/2 keyboard — IOAPIC-routed, LAPIC-delivered ─────────────────
; IRQ1 is now delivered by the IOAPIC → LAPIC at vector 0x21.
; EOI to LAPIC only (PIC is disabled).
global irq1_entry
irq1_entry:
    PUSH_GPRS
    call lapic_eoi
    call keyboard_irq_handler
    POP_GPRS
    iretq

; ── 0x2C: PS/2 mouse — IOAPIC-routed, LAPIC-delivered ───────────────────
; IRQ12 is now delivered by the IOAPIC → LAPIC at vector 0x2C.
; EOI to LAPIC only.
global irq12_entry
irq12_entry:
    PUSH_GPRS
    call lapic_eoi
    call mouse_irq_handler
    POP_GPRS
    iretq

; ── VEC_AHCI_MSI (0x32): AHCI MSI ────────────────────────────────────────
; MSI bypasses the IOAPIC entirely; delivered directly to LAPIC.
; EOI to LAPIC only.
global ahci_irq_entry
ahci_irq_entry:
    PUSH_GPRS
    call lapic_eoi
    call ahci_irq_handler
    POP_GPRS
    iretq

; ── VEC_HDA_MSI (0x33): HDA MSI ──────────────────────────────────────────
global hda_irq_entry
hda_irq_entry:
    PUSH_GPRS
    call lapic_eoi
    call hda_irq_handler
    POP_GPRS
    iretq

; ── Spurious LAPIC vector (0x30) ─────────────────────────────────────────
; The LAPIC fires the spurious vector if an interrupt is cancelled before
; delivery (e.g. during an EOI race).  Per spec: do NOT send EOI for a
; spurious interrupt — just return.
global lapic_spurious_entry
lapic_spurious_entry:
    iretq
