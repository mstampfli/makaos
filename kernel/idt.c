#include "idt.h"
#include "signal.h"
#include "sched.h"

volatile uint16_t* g_vga = (volatile uint16_t*)(VGA_ADDR + HHDM_OFFSET);

__attribute__((aligned(16)))
static idt_gate_t idt[256] = {0};

static inline void lidt(void* base, uint16_t size_minus_1) {
    volatile idtr_t idtr = { .limit = size_minus_1, .base = (uint64_t)base };
    __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

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

void isr_general_exception_no_ec(const char* msg, interrupt_frame_t* frame) {

    volatile uint16_t* v = g_vga; //VGA
                                               
    //for (int i = 0; i < 80 * 25; i++) v[i] = (uint16_t)' ' | (0x07 << 8);

    for (uint32_t i = 0; msg[i] != '\0'; i++) {
        v[i] = (uint16_t)msg[i] | (uint16_t)(0x07 << 8);
    }

    vga_hex_draw(v, frame->ip, 2);
    vga_hex_draw(v, frame->sp, 3);
    vga_hex_draw(v, frame->flags, 4);

    for (;;) { __asm__ __volatile__("cli; hlt"); }
}

static void serial_putc_idt(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts_idt(const char* s) {
    for (; *s; s++) serial_putc_idt(*s);
}
static void serial_hex_idt(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (uint8_t)((v >> i) & 0xF);
        serial_putc_idt(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    serial_putc_idt('\n');
}

void isr_general_exception_ec(const char* msg, interrupt_frame_t* frame, uint64_t error_code) {
    volatile uint16_t* v = g_vga; //VGA

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

    // Also dump to serial for headless debugging.
    serial_puts_idt("EXCEPTION: ");
    serial_puts_idt(msg);
    serial_putc_idt('\n');
    serial_puts_idt("ec="); serial_hex_idt(error_code);
    serial_puts_idt("ip="); serial_hex_idt(frame->ip);
    serial_puts_idt("cs="); serial_hex_idt(frame->cs);
    serial_puts_idt("sp="); serial_hex_idt(frame->sp);

    for (;;) { __asm__ __volatile__("cli; hlt"); }
}

// ── Exception dispatch helpers ────────────────────────────────────────────
// If the fault came from user mode (CS & 3 == 3), send a signal and return.
// If from kernel mode, halt (kernel bug — unrecoverable).

static inline uint8_t from_user(interrupt_frame_t* f) {
    return (f->cs & 3) == 3;
}

static void user_signal_or_halt_noec(interrupt_frame_t* f, int sig, const char* msg) {
    if (from_user(f)) {
        signal_send(g_current, sig);
        signal_deliver_pending();
        return;
    }
    isr_general_exception_no_ec(msg, f);
}

static void idc_ser_c(char c) { while (!(inb(0x3F8+5)&0x20)); outb(0x3F8,(uint8_t)c); }
static void idc_ser_hex(uint64_t v) {
    const char* h="0123456789ABCDEF";
    idc_ser_c('0'); idc_ser_c('x');
    for(int i=0;i<16;i++) idc_ser_c(h[(v>>(60-i*4))&0xF]);
}
static void ser_dbg_ec(const char* tag, interrupt_frame_t* f, uint64_t ec) {
    for(const char*p=tag;*p;p++) idc_ser_c(*p);
    idc_ser_c(' '); idc_ser_c('e'); idc_ser_c('c'); idc_ser_c('=');
    idc_ser_hex(ec);
    idc_ser_c(' '); idc_ser_c('R'); idc_ser_c('I'); idc_ser_c('P'); idc_ser_c('=');
    idc_ser_hex(f->ip);
    idc_ser_c(' '); idc_ser_c('R'); idc_ser_c('S'); idc_ser_c('P'); idc_ser_c('=');
    idc_ser_hex(f->sp); idc_ser_c('\n');
}

static void user_signal_or_halt_ec(interrupt_frame_t* f, uint64_t ec, int sig, const char* msg) {
    if (from_user(f)) {
        ser_dbg_ec(msg, f, ec);
        signal_send(g_current, sig);
        signal_deliver_pending();
        return;
    }
    isr_general_exception_ec(msg, f, ec);
}

void isr0_divide_error(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGFPE,  "#DE Divide Error"); }
void isr1_debug(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGILL,  "#DB Debug"); }
void isr2_nmi(interrupt_frame_t* f)
    { isr_general_exception_no_ec("Non-Maskable Interrupt", f); } // NMI always halt
void isr5_bound(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGSEGV, "#BR BOUND Range Exceeded"); }
void isr6_invalid_opcode(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGILL,  "#UD Invalid Opcode"); }
void isr7_device_na(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGILL,  "#NM Device Not Available"); }
void isr8_double_fault(interrupt_frame_t* f, uint64_t ec)
    { isr_general_exception_ec("#DF Double Fault", f, ec); } // always kernel halt
void isr9_coprocessor_overrun(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGILL,  "Coprocessor Segment Overrun"); }
void isr10_invalid_tss(interrupt_frame_t* f, uint64_t ec)
    { isr_general_exception_ec("#TS Invalid TSS", f, ec); }
void isr11_seg_np(interrupt_frame_t* f, uint64_t ec)
    { user_signal_or_halt_ec(f, ec, SIGSEGV, "#NP Segment Not Present"); }
void isr12_stack_fault(interrupt_frame_t* f, uint64_t ec)
    { user_signal_or_halt_ec(f, ec, SIGSEGV, "#SS Stack Fault"); }
void isr13_gp(interrupt_frame_t* f, uint64_t ec)
    { user_signal_or_halt_ec(f, ec, SIGSEGV, "#GP General Protection"); }
// Vector 14: page fault — handled in vmm.c (sends SIGSEGV via kill_current)
void isr16_x87_fp(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGFPE,  "#MF x87 FPU FP Error"); }
void isr17_alignment(interrupt_frame_t* f, uint64_t ec)
    { user_signal_or_halt_ec(f, ec, SIGBUS, "#AC Alignment Check"); }
void isr18_machine_check(interrupt_frame_t* f)
    { isr_general_exception_no_ec("#MC Machine Check", f); } // always halt
void isr19_simd_fp(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGFPE,  "#XM SIMD FP Exception"); }
void isr20_virtualization(interrupt_frame_t* f)
    { user_signal_or_halt_noec(f, SIGILL,  "#VE Virtualization Exception"); }
void isr21_control_protection(interrupt_frame_t* f, uint64_t ec)
    { user_signal_or_halt_ec(f, ec, SIGILL, "#CP Control Protection"); }

static void idt_gate_set(uint8_t vec, uint64_t addr, uint16_t selector, uint8_t ist_index, uint8_t type_attr) {
    idt[vec].handler_offset_low = (uint16_t)(addr & 0xFFFFu);
    idt[vec].segment_selector = selector;
    idt[vec].ist = (uint8_t)(ist_index & 0x7u);
    idt[vec].type = type_attr;
    idt[vec].handler_offset_mid = (uint16_t)((addr >> 16) & 0xFFFFu);
    idt[vec].handler_offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFFu);
    idt[vec].reserved = 0;
}

void idt_init(void) {
    const uint8_t ist0 = 0;
    const uint8_t ist1 = 1; // IST1 → #DF: dedicated stack so a stack-overflow
                             //             double fault doesn't triple-fault
    const uint8_t ist2 = 2; // IST2 → #NMI: NMI must not share stack with
                             //              whatever code it interrupted

    idt_gate_set(0,  (uint64_t)isr0_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(1,  (uint64_t)isr1_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(2,  (uint64_t)isr2_entry, KERNEL_CS, ist2, IDT_ATTR_INTGATE);
    idt_gate_set(5,  (uint64_t)isr5_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(6,  (uint64_t)isr6_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(7,  (uint64_t)isr7_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(8,  (uint64_t)isr8_entry, KERNEL_CS, ist1, IDT_ATTR_INTGATE);
    idt_gate_set(9,  (uint64_t)isr9_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(10, (uint64_t)isr10_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(11, (uint64_t)isr11_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(12, (uint64_t)isr12_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(13, (uint64_t)isr13_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(14, (uint64_t)isr14_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(16, (uint64_t)isr16_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(17, (uint64_t)isr17_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(18, (uint64_t)isr18_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(19, (uint64_t)isr19_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(20, (uint64_t)isr20_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);
    idt_gate_set(21, (uint64_t)isr21_entry, KERNEL_CS, ist0, IDT_ATTR_INTGATE);

    lidt(idt, sizeof(idt) - 1);
}

void idt_irq_register(uint8_t vec, uint64_t handler_addr) {
    idt_gate_set(vec, handler_addr, KERNEL_CS, 0, IDT_ATTR_INTGATE);
}
