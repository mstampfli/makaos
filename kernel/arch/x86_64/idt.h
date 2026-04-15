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
