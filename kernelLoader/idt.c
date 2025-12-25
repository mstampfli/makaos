#include "idt.h"

__attribute__((aligned(16)))
static idt_gate_t idt[256] = {0};

static inline void lidt(void* base, uint16_t size_minus_1) {
    volatile idtr_t idtr = { .limit = size_minus_1, .base = (uint64_t)base };
    __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

__attribute__((no_caller_saved_registers))
static void vga_hex_draw(volatile uint16_t* v, uint64_t value, int row) {
    const char* hex_chars = "0123456789ABCDEF";
    int offset = row * 80; // Standard 80-column VGA mode
    
    // Prefix "0x"
    v[offset++] = (uint16_t)'0' | (0x07 << 8);
    v[offset++] = (uint16_t)'x' | (0x07 << 8);

    // Process 16 nibbles for 64-bit value
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        v[offset++] = (uint16_t)hex_chars[nibble] | (0x07 << 8);
    }
}

__attribute__((no_caller_saved_registers))
static void isr_general_exception_no_ec(const char* msg, interrupt_frame_t* frame) {

    volatile uint16_t* v = (uint16_t*)0xB8000; //VGA
                                               
    for (int i = 0; i < 80 * 25; i++) v[i] = (uint16_t)' ' | (0x07 << 8);

    for (uint32_t i = 0; msg[i] != '\0'; i++) {
        v[i] = (uint16_t)msg[i] | (uint16_t)(0x07 << 8);
    }

    vga_hex_draw(v, frame->ip, 2);
    vga_hex_draw(v, frame->sp, 3);
    vga_hex_draw(v, frame->flags, 4);

    for (;;) { __asm__ __volatile__("cli; hlt"); }
}

__attribute__((no_caller_saved_registers))
static void isr_general_exception_ec(const char* msg, interrupt_frame_t* frame, uint64_t error_code) {
    volatile uint16_t* v = (uint16_t*)0xB8000;
    
    // Clear screen
    for (int i = 0; i < 80 * 25; i++) v[i] = (uint16_t)' ' | (0x07 << 8);
    
    // Print message
    for (uint32_t i = 0; msg[i] != '\0'; i++) {
        v[i] = (uint16_t)msg[i] | (0x07 << 8);
    }
    
    // Print error code and CR2
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    
    vga_hex_draw(v, error_code, 1);  // Error code
    vga_hex_draw(v, cr2, 2);          // CR2 (faulting address)
    vga_hex_draw(v, frame->ip, 3);    // RIP
    vga_hex_draw(v, (uint64_t)frame, 4); // Frame pointer itself
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    vga_hex_draw(v, rsp, 5);          // Current RSP
    
    for (;;) { __asm__ __volatile__("cli; hlt"); }
}

__attribute__((interrupt)) void isr0_divide_error(interrupt_frame_t* f) { isr_general_exception_no_ec("#DE Divide Error", f); }
__attribute__((interrupt)) void isr1_debug(interrupt_frame_t* f)        { isr_general_exception_no_ec("#DB Debug", f); }
__attribute__((interrupt)) void isr2_nmi(interrupt_frame_t* f)          { isr_general_exception_no_ec("Non-Maskable Interrupt", f); }
// Vector 3,4 only for software interrupts
__attribute__((interrupt)) void isr5_bound(interrupt_frame_t* f)        { isr_general_exception_no_ec("#BR BOUND Range Exceeded", f); }
__attribute__((interrupt)) void isr6_invalid_opcode(interrupt_frame_t* f){ isr_general_exception_no_ec("#UD Invalid Opcode", f); }
__attribute__((interrupt)) void isr7_device_na(interrupt_frame_t* f)    { isr_general_exception_no_ec("#NM Device Not Available", f); }
__attribute__((interrupt)) void isr8_double_fault(interrupt_frame_t* f, uint64_t ec) { isr_general_exception_ec("#DF Double Fault", f, ec); }
__attribute__((interrupt)) void isr9_coprocessor_overrun(interrupt_frame_t* f) { isr_general_exception_no_ec("Coprocessor Segment Overrun", f); }
__attribute__((interrupt)) void isr10_invalid_tss(interrupt_frame_t* f, uint64_t ec) { isr_general_exception_ec("#TS Invalid TSS", f, ec); }
__attribute__((interrupt)) void isr11_seg_np(interrupt_frame_t* f, uint64_t ec)     { isr_general_exception_ec("#NP Segment Not Present", f, ec); }
__attribute__((interrupt)) void isr12_stack_fault(interrupt_frame_t* f, uint64_t ec){ isr_general_exception_ec("#SS Stack Fault", f, ec); }
__attribute__((interrupt)) void isr13_gp(interrupt_frame_t* f, uint64_t ec)         { isr_general_exception_ec("#GP General Protection", f, ec); }
__attribute__((interrupt)) void isr14_page_fault(interrupt_frame_t* f, uint64_t ec) { isr_general_exception_ec("#PF Page Fault", f, ec); }
// Vector 15 reserved
__attribute__((interrupt)) void isr16_x87_fp(interrupt_frame_t* f)      { isr_general_exception_no_ec("#MF x87 FPU FP Error", f); }
__attribute__((interrupt)) void isr17_alignment(interrupt_frame_t* f, uint64_t ec)  { isr_general_exception_ec("#AC Alignment Check", f, ec); }
__attribute__((interrupt)) void isr18_machine_check(interrupt_frame_t* f){ isr_general_exception_no_ec("#MC Machine Check", f); }
__attribute__((interrupt)) void isr19_simd_fp(interrupt_frame_t* f)      { isr_general_exception_no_ec("#XM SIMD FP Exception", f); }
__attribute__((interrupt)) void isr20_virtualization(interrupt_frame_t* f){ isr_general_exception_no_ec("#VE Virtualization Exception", f); }
__attribute__((interrupt)) void isr21_control_protection(interrupt_frame_t* f, uint64_t ec) { isr_general_exception_ec("#CP Control Protection", f, ec); }

static void idt_gate_set(uint8_t vec, uint64_t addr, uint16_t selector, uint8_t ist_index, uint8_t type_attr) {
    idt[vec].handler_offset_low  = (uint16_t)(addr & 0xFFFFu);
    idt[vec].segment_selector       = selector;
    idt[vec].ist                   = (uint8_t)(ist_index & 0x7u);
    idt[vec].type                  = type_attr;
    idt[vec].handler_offset_mid = (uint16_t)((addr >> 16) & 0xFFFFu);
    idt[vec].handler_offset_high   = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    idt[vec].reserved                = 0;
}

void idt_init(void) {
    const uint8_t  ist0 = 0;

    idt_gate_set(0,  (uint64_t)isr0_divide_error,    KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(1,  (uint64_t)isr1_debug,           KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(2,  (uint64_t)isr2_nmi,             KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(5,  (uint64_t)isr5_bound,           KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(6,  (uint64_t)isr6_invalid_opcode,  KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(7,  (uint64_t)isr7_device_na,       KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(8,  (uint64_t)isr8_double_fault,    KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(9,  (uint64_t)isr9_coprocessor_overrun, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(10, (uint64_t)isr10_invalid_tss,    KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(11, (uint64_t)isr11_seg_np,         KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(12, (uint64_t)isr12_stack_fault,    KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(13, (uint64_t)isr13_gp,             KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(14, (uint64_t)isr14_page_fault,     KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(16, (uint64_t)isr16_x87_fp,         KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(17, (uint64_t)isr17_alignment,      KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(18, (uint64_t)isr18_machine_check,  KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(19, (uint64_t)isr19_simd_fp,        KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(20, (uint64_t)isr20_virtualization, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(21, (uint64_t)isr21_control_protection, KERNEL_CS, ist0, IDT_ATTR_INTGATE);

    lidt(idt, sizeof(idt) - 1);
}
