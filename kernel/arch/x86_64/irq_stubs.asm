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
extern virtio_net_irq_handler
extern ipi_reschedule_handler
extern ipi_call_handler
extern ipi_tlb_flush_handler
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
; IRQ1 is masked at the IOAPIC until keyboard_init() installs this handler,
; flushes the KBC, and calls ioapic_unmask().  No stub needed.
global irq1_entry
irq1_entry:
    PUSH_GPRS
    call lapic_eoi
    call keyboard_irq_handler
    POP_GPRS
    iretq

; ── 0x2C: PS/2 mouse — IOAPIC-routed, LAPIC-delivered ───────────────────
; IRQ12 is masked at the IOAPIC until mouse_init() installs this handler
; and calls ioapic_unmask().  No stub needed.
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

; ── VEC_VIRTIO_NET (0x34): virtio-net MSI ────────────────────────────────
; Shared RX+TX vector.  MSI bypasses IOAPIC; EOI to LAPIC only.
global virtio_net_irq_entry
virtio_net_irq_entry:
    PUSH_GPRS
    call lapic_eoi
    call virtio_net_irq_handler
    POP_GPRS
    iretq

; ── Spurious LAPIC vector (0x30) ─────────────────────────────────────────
; The LAPIC fires the spurious vector if an interrupt is cancelled before
; delivery (e.g. during an EOI race).  Per spec: do NOT send EOI for a
; spurious interrupt — just return.
global lapic_spurious_entry
lapic_spurious_entry:
    iretq

; ── VEC_IPI_RESCHEDULE (0x40) ────────────────────────────────────────────
; Sent by sched_wake / sched_add when a runnable task has been placed on
; this CPU's run queue by a remote CPU.  The handler sets
; this_cpu()->reschedule_pending, then the tail of this stub calls
; sched_preempt() which actually runs the context switch if we're at
; a preemptible point (same pattern as timer_irq_handler).
;
; IMPORTANT: this stub may be entered while the victim CPU was sitting
; in `sti; hlt` inside do_switch's idle loop.  That's the whole point —
; the hlt wakes, the handler runs, sched_preempt picks up the new task,
; we context-switch away from the idle caller.  The caller's saved RSP
; (inside do_switch's prologue) gets restored on the way back, but by
; the time we iret out we are on a completely different task's stack,
; because do_switch's loop re-enters on the returned-to stack and then
; its own spin_lock_irqsave+dequeue_local picks up the task.
global ipi_reschedule_entry
ipi_reschedule_entry:
    PUSH_GPRS
    call lapic_eoi
    call ipi_reschedule_handler
    POP_GPRS
    iretq

; ── VEC_IPI_CALL (0x42) ──────────────────────────────────────────────────
; Generic cross-CPU function call: drain this CPU's MPSC call queue.
; One wrmsr of sender → handler here → sender spins on per-call done flag.
global ipi_call_entry
ipi_call_entry:
    PUSH_GPRS
    call lapic_eoi
    call ipi_call_handler
    POP_GPRS
    iretq

; ── VEC_IPI_TLB_FLUSH (0x41) ─────────────────────────────────────────────
; Phase 9-6 owns the real implementation; today this stub exists so a
; stray fire doesn't land on the generic #GP path.  Handler drains the
; CPU's shootdown slot and invlpg's the range (Phase 9-6).  For Phase
; 9-5 the handler is an EOI-only no-op.
global ipi_tlb_flush_entry
ipi_tlb_flush_entry:
    PUSH_GPRS
    call lapic_eoi
    call ipi_tlb_flush_handler
    POP_GPRS
    iretq

