#include "pic.h"

// Small I/O delay: writing to port 0x80 (POST diagnostic port) is safe on
// all x86 hardware and gives enough time for old ISA devices to respond.
static inline void io_wait(void) { outb(0x80, 0); }

void pic_init(uint8_t base_master, uint8_t base_slave) {
    // ICW1 (0x11): start initialisation sequence, expect ICW4.
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();

    // ICW2: vector base offset for each PIC.
    outb(PIC1_DATA, base_master); io_wait();
    outb(PIC2_DATA, base_slave);  io_wait();

    // ICW3: cascade wiring.
    // Master: slave is connected on IRQ2 (bit 2 = 0x04).
    // Slave: its cascade identity line is 2.
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    // ICW4: 8086/88 mode.
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    // Mask all IRQ lines.  Callers unmask only what they need.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    // Slave needs its own EOI first, then master acknowledges the cascade.
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) | (uint8_t)(1u << bit));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) & (uint8_t)~(1u << bit));
}
