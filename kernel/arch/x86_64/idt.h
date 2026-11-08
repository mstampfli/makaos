#pragma once
#include "common.h"

// interrupt gate: present=1, dpl=0, type=0xe (64-bit interrupt gate)
#define IDT_ATTR_INTGATE 0x8E

typedef struct interrupt_frame_t {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

// ── Full exception trap frame (the entire on-stack register save) ─────────
// isr_common_entry (isr_stubs.asm) builds this on the kernel stack for every
// exception and passes the C handler a pointer to its `ip` field (i.e. an
// interrupt_frame_t* overlaying the tail).  The GPRs are pushed by PUSH_GPRS
// (rax at the lowest address), then the ISR_EC/ISR_NOEC macros push
// {has_ec, handler, ec}, then the CPU-pushed {ip,cs,flags,sp,ss}.  The C
// handler receives &tf->ip; TRAP_FROM_IFRAME recovers the whole frame so
// signal delivery can read/redirect the faulting GPRs.  Because the stub now
// passes a pointer to the REAL frame (not a copy), edits to any field here
// take effect on the POP_GPRS + iretq return path — this is how a synchronous
// fault delivers a signal to a user handler (see signal_deliver_fault).
//
// The field order and offsets MUST match isr_stubs.asm exactly; the
// _Static_asserts below pin the contract so an asm/layout drift fails the
// build instead of corrupting register state at fault time.
typedef struct trap_frame_t {
    uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;   // PUSH_GPRS (rax lowest)
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t has_ec, handler, ec;                 // ISR_EC/ISR_NOEC macros
    uint64_t ip, cs, flags, sp, ss;               // CPU-pushed (interrupt_frame_t)
} __attribute__((packed)) trap_frame_t;

_Static_assert(sizeof(trap_frame_t) == 23 * 8, "trap_frame_t is 23 qwords");
_Static_assert(__builtin_offsetof(trap_frame_t, ip) == 18 * 8,
               "ip at GPRbase+144 (15 GPRs + has_ec/handler/ec)");
_Static_assert(__builtin_offsetof(trap_frame_t, rax) == 0, "rax at frame base");

// Recover the full trap frame from the interrupt_frame_t* the C handler got.
#define TRAP_FROM_IFRAME(f) \
    ((trap_frame_t*)((uint8_t*)(f) - __builtin_offsetof(trap_frame_t, ip)))

typedef struct idtr_t {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;



typedef struct idt_gate_t {
    uint16_t handler_offset_low;
    uint16_t segment_selector;
    uint8_t  ist;
    uint8_t  type;
    uint16_t handler_offset_mid;
    uint32_t handler_offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_gate_t;

// initializes descriptors and loads idtr
void idt_init(void);

// AP-side IDT load: APs share the BSP's vector table, they only need
// to lidt their own IDTR at it.  Call after idt_init() has run on the BSP.
void idt_load_ap(void);

// Register an IRQ handler at the given vector (e.g. 0x20 for IRQ0).
// Used by timer drivers and future device drivers.
void idt_irq_register(uint8_t vec, uint64_t handler_addr);
void isr_general_exception_no_ec(const char* msg, interrupt_frame_t* frame);
void isr_general_exception_ec(const char* msg, interrupt_frame_t* frame, uint64_t error_code);

extern void isr0_entry(void);
extern void isr1_entry(void);
extern void isr2_entry(void);

extern void isr5_entry(void);
extern void isr6_entry(void);
extern void isr7_entry(void);

extern void isr8_entry(void);
extern void isr9_entry(void);

extern void isr10_entry(void);
extern void isr11_entry(void);
extern void isr12_entry(void);
extern void isr13_entry(void);
extern void isr14_entry(void);

extern void isr16_entry(void);
extern void isr17_entry(void);
extern void isr18_entry(void);
extern void isr19_entry(void);
extern void isr20_entry(void);
extern void isr21_entry(void);
