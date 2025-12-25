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



typedef struct idt_entry_t {
    uint16_t handler_offset_low;
    uint16_t segment_selector;
    uint8_t  ist;
    uint8_t  type;
    uint16_t handler_offset_mid;
    uint32_t handler_offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;
