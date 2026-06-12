#include "idt.h"
#include "signal.h"
#include "sched.h"
#include "tss.h"
#include "cpu.h"
#include "fb.h"
#include "panic.h"

/* Kept as NULL for UEFI boots — legacy symbol referenced by signal.c */
volatile uint16_t* g_vga = (volatile uint16_t*)0;

__attribute__((aligned(16)))
static idt_gate_t idt[256] = {0};

static inline void lidt(void* base, uint16_t size_minus_1) {
    volatile idtr_t idtr = { .limit = size_minus_1, .base = (uint64_t)base };
    __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

/* ── Panic output helpers (serial + framebuffer) ─────────────────────────── */
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

/* Write a line to the framebuffer at a fixed row (panic context, no scroll) */
static void fb_panic_str(uint32_t row, const char* s, uint32_t fg) {
    if (!g_fb.base_virt) return;
    for (uint32_t col = 0; s[col] && col < fb_cols(); col++)
        fb_putc_at(col, row, s[col], fg, FB_BLACK);
}

static void fb_panic_hex(uint32_t row, uint32_t col_start, uint64_t v) {
    if (!g_fb.base_virt) return;
    const char* hex = "0123456789ABCDEF";
    char buf[19]; /* "0x" + 16 digits + NUL */
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    for (uint32_t i = 0; buf[i] && col_start + i < fb_cols(); i++)
        fb_putc_at(col_start + i, row, buf[i], FB_YELLOW, FB_BLACK);
}

void isr_general_exception_no_ec(const char* msg, interrupt_frame_t* frame) {
    /* FB sidebar stays as-is — some failures happen before serial is
     * worth reading and the FB banner is the only visible signal. */
    fb_panic_str(0, msg,       FB_LRED);
    fb_panic_str(1, "RIP=",    FB_GRAY); fb_panic_hex(1, 4, frame->ip);
    fb_panic_str(2, "RSP=",    FB_GRAY); fb_panic_hex(2, 4, frame->sp);
    fb_panic_str(3, "RFLAGS=", FB_GRAY); fb_panic_hex(3, 7, frame->flags);

    /* Rich dump via the debug-subsystem panic (registers, backtrace,
     * log ring, trace ring, halts other CPUs). */
    panic_from_exception(msg, frame, 0, /*has_ec=*/0);
}

void isr_general_exception_ec(const char* msg, interrupt_frame_t* frame, uint64_t error_code) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    fb_panic_str(0, msg,      FB_LRED);
    fb_panic_str(1, "ec=",    FB_GRAY); fb_panic_hex(1, 3, error_code);
    fb_panic_str(2, "CR2=",   FB_GRAY); fb_panic_hex(2, 4, cr2);
    fb_panic_str(3, "RIP=",   FB_GRAY); fb_panic_hex(3, 4, frame->ip);
    fb_panic_str(4, "RSP=",   FB_GRAY); fb_panic_hex(4, 4, frame->sp);

    panic_from_exception(msg, frame, error_code, /*has_ec=*/1);
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
        signal_deliver_pending(0);
        return;
    }
    isr_general_exception_no_ec(msg, f);
}

static void user_signal_or_halt_ec(interrupt_frame_t* f, uint64_t ec, int sig, const char* msg) {
    if (from_user(f)) {
        serial_puts_idt(msg); serial_putc_idt(' ');
        serial_puts_idt("ec="); serial_hex_idt(ec);
        serial_puts_idt("ip="); serial_hex_idt(f->ip);
        signal_send(g_current, sig);
        signal_deliver_pending(0);
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
void isr13_gp(interrupt_frame_t* f, uint64_t ec) {
    // Rich GP diagnostics — always dump to serial regardless of user/kernel.
    serial_puts_idt("\n=== #GP FAULT ===\n");
    serial_puts_idt("ec=");   serial_hex_idt(ec);
    serial_puts_idt("RIP=");  serial_hex_idt(f->ip);
    serial_puts_idt("CS=");   serial_hex_idt(f->cs);
    serial_puts_idt("FLG=");  serial_hex_idt(f->flags);
    serial_puts_idt("RSP=");  serial_hex_idt(f->sp);
    serial_puts_idt("SS=");   serial_hex_idt(f->ss);
    serial_puts_idt("RSP0="); serial_hex_idt(this_cpu()->tss.rsp[0]);

    // Error code breakdown: [15:3]=selector index, [2]=TI, [1]=IDT, [0]=EXT
    if (ec) {
        serial_puts_idt("ec.idx="); serial_hex_idt((ec >> 3) & 0x1FFF);
        serial_puts_idt("ec.TI=");  serial_hex_idt((ec >> 2) & 1);
        serial_puts_idt("ec.IDT="); serial_hex_idt((ec >> 1) & 1);
        serial_puts_idt("ec.EXT="); serial_hex_idt(ec & 1);
    }

    // Process info
    if (g_current) {
        serial_puts_idt("PID=");  serial_hex_idt(g_current->pid);
        serial_puts_idt("PPID="); serial_hex_idt(g_current->ppid);
        serial_puts_idt("KSTK="); serial_hex_idt(g_current->kstack_top);
        serial_puts_idt("CMD=");
        serial_puts_idt(g_current->comm);
        serial_putc_idt('\n');
    }

    // Dump 8 qwords at the faulting RSP (if accessible)
    serial_puts_idt("STACK@RSP:\n");
    uint64_t* stk = (uint64_t*)f->sp;
    for (int i = 0; i < 8; i++) {
        serial_hex_idt(stk[i]);
    }
    serial_puts_idt("=== END #GP ===\n");

    if (from_user(f)) {
        signal_send(g_current, SIGSEGV);
        signal_deliver_pending(0);
        return;
    }
    // Kernel GP — halt
    fb_panic_str(0, "#GP General Protection", FB_LRED);
    fb_panic_str(1, "ec=",  FB_GRAY); fb_panic_hex(1, 3, ec);
    fb_panic_str(2, "RIP=", FB_GRAY); fb_panic_hex(2, 4, f->ip);
    fb_panic_str(3, "RSP=", FB_GRAY); fb_panic_hex(3, 4, f->sp);
    fb_panic_str(4, "SS=",  FB_GRAY); fb_panic_hex(4, 3, f->ss);
    for (;;) { __asm__ __volatile__("cli; hlt"); }
}
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

// AP-side IDT load — the vector table itself is shared, so APs only need
// to point IDTR at it.  Called from cpu_init_ap after the shared GDT is
// installed; the BSP already ran idt_init() which populated idt[].
void idt_load_ap(void) {
    lidt(idt, sizeof(idt) - 1);
}
