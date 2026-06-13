#include "mouse.h"
#include "idt.h"
#include "ioapic.h"
#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "preempt.h"
#include "cpu.h"
#include "input_core.h"   // g_i8042_lock — shared with the keyboard ISR

// Global wait queue — woken after each decoded mouse packet so poll/epoll
// on /dev/mouse wakes immediately when events are available.
wait_queue_t g_mouse_waitq;

// ── PS/2 KBC ports ───────────────────────────────────────────────────────
#define KBC_DATA    0x60   // read data / write aux data
#define KBC_STATUS  0x64   // read status register
#define KBC_CMD     0x64   // write command

// KBC status register bits
#define KBC_STATUS_OBF  (1 << 0)   // output buffer full (data ready to read)
#define KBC_STATUS_IBF  (1 << 1)   // input buffer full  (not ready to accept)
#define KBC_STATUS_AUX  (1 << 5)   // output buffer data is from aux (mouse)

// KBC commands
#define KBC_CMD_WRITE_AUX    0xD4  // next byte written to 0x60 goes to mouse
#define KBC_CMD_ENABLE_AUX   0xA8  // enable auxiliary PS/2 port
#define KBC_CMD_READ_CCB     0x20  // read Controller Command Byte
#define KBC_CMD_WRITE_CCB    0x60  // write Controller Command Byte

// PS/2 mouse commands (sent via KBC_CMD_WRITE_AUX)
#define MOUSE_CMD_RESET          0xFF
#define MOUSE_CMD_ENABLE_REPORT  0xF4  // enable data reporting (stream mode)
#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_SET_SAMPLE     0xF3  // followed by sample rate byte
#define MOUSE_ACK                0xFA

// ── PS/2 packet accumulator ───────────────────────────────────────────────
// A standard PS/2 mouse sends 3-byte packets.  The ISR accumulates bytes
// one at a time into a staging buffer; a complete packet is pushed to the
// ring buffer atomically.

#define PACKET_BYTES 3

// Packet byte 0 (flags) bit definitions
#define PKT_BTN_LEFT   (1 << 0)
#define PKT_BTN_RIGHT  (1 << 1)
#define PKT_BTN_MIDDLE (1 << 2)
#define PKT_Y_SIGN     (1 << 5)   // sign bit for dy (1 = negative in PS/2 coords)
#define PKT_X_SIGN     (1 << 4)   // sign bit for dx
#define PKT_X_OVERFLOW (1 << 6)
#define PKT_Y_OVERFLOW (1 << 7)
// bit 3 is always 1 in a valid first byte — used for sync

static uint8_t  s_packet[PACKET_BYTES];
static uint8_t  s_packet_idx = 0;

// ── Event ring buffer ─────────────────────────────────────────────────────
// ISR → thread boundary; power-of-2 so modulo compiles to AND.
#define EVT_BUF_SIZE 128   // must be power of 2
static volatile mouse_event_t s_evt_buf[EVT_BUF_SIZE];
static volatile uint8_t       s_evt_head = 0;  // consumer reads here
static volatile uint8_t       s_evt_tail = 0;  // producer writes here

static void evt_push(mouse_event_t ev) {
    uint8_t next = (s_evt_tail + 1) & (EVT_BUF_SIZE - 1);
    if (next == s_evt_head) return;  // full — drop oldest event
    s_evt_buf[s_evt_tail] = ev;
    s_evt_tail = next;
}

// ── KBC helpers ───────────────────────────────────────────────────────────

static void kbc_wait_write(void) {
    // Spin until KBC input buffer is empty (safe to write).
    while (inb(KBC_STATUS) & KBC_STATUS_IBF);
}

static void kbc_wait_read(void) {
    // Spin until KBC output buffer has data.
    while (!(inb(KBC_STATUS) & KBC_STATUS_OBF));
}

static void kbc_flush(void) {
    // Drain any stale bytes from the KBC output buffer.
    while (inb(KBC_STATUS) & KBC_STATUS_OBF)
        inb(KBC_DATA);
}

// Send a byte to the PS/2 mouse via the KBC.
static void mouse_write(uint8_t byte) {
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_WRITE_AUX);
    kbc_wait_write();
    outb(KBC_DATA, byte);
}

// Read one byte from the KBC output buffer (blocking).
static uint8_t mouse_read_byte(void) {
    kbc_wait_read();
    return inb(KBC_DATA);
}

// ── Packet decode ─────────────────────────────────────────────────────────

// Takes the packet as a snapshot copied out under g_i8042_lock — it must
// NOT read s_packet[] directly: decode runs outside the lock, and a
// concurrent ISR on another CPU may already be accumulating the next
// packet into the shared buffer.
static void packet_decode(const uint8_t* pkt) {
    uint8_t flags = pkt[0];
    uint8_t raw_x = pkt[1];
    uint8_t raw_y = pkt[2];

    // Discard packets with overflow bits set — values are unreliable.
    if (flags & (PKT_X_OVERFLOW | PKT_Y_OVERFLOW)) return;

    // Sign-extend 9-bit values: sign bit is in the flags byte.
    int16_t dx = (int16_t)raw_x;
    int16_t dy = (int16_t)raw_y;
    if (flags & PKT_X_SIGN) dx |= 0xFF00;   // extend sign
    if (flags & PKT_Y_SIGN) dy |= 0xFF00;

    // PS/2 Y axis is inverted relative to screen coordinates.
    dy = -dy;

    mouse_event_t ev;
    ev.dx      = dx;
    ev.dy      = dy;
    ev.buttons = 0;
    if (flags & PKT_BTN_LEFT)   ev.buttons |= MOUSE_BTN_LEFT;
    if (flags & PKT_BTN_RIGHT)  ev.buttons |= MOUSE_BTN_RIGHT;
    if (flags & PKT_BTN_MIDDLE) ev.buttons |= MOUSE_BTN_MIDDLE;

    evt_push(ev);

    // Bridge to the evdev layer so /dev/input/event1 (mouse) fires
    // EV_REL / EV_KEY / SYN_REPORT for libinput consumers.  The native
    // /dev/mouse ring above feeds the legacy compositor path unchanged.
    extern void evdev_on_mouse_packet(int32_t dx, int32_t dy, uint8_t buttons);
    uint8_t evdev_buttons = 0;
    if (flags & PKT_BTN_LEFT)   evdev_buttons |= 0x1;
    if (flags & PKT_BTN_MIDDLE) evdev_buttons |= 0x2;
    if (flags & PKT_BTN_RIGHT)  evdev_buttons |= 0x4;
    evdev_on_mouse_packet((int32_t)dx, (int32_t)dy, evdev_buttons);
}

// ── ISR byte sink + packet completion (called from i8042_isr_drain) ──────

// Accumulate one AUX byte (caller holds g_i8042_lock).  Returns 1 with
// pkt_out[3] filled when a 3-byte packet completes.
int mouse_isr_byte(uint8_t byte, uint8_t* pkt_out) {
    // Sync: the first byte of a packet always has bit 3 set.
    // If we're out of sync, wait for a valid first byte.
    if (s_packet_idx == 0 && !(byte & (1 << 3)))
        return 0;
    // Belt-and-braces clamp: whatever desync history led here, the
    // index can never escape the buffer.
    if (s_packet_idx >= PACKET_BYTES)
        s_packet_idx = 0;
    s_packet[s_packet_idx++] = byte;
    if (s_packet_idx >= PACKET_BYTES) {
        s_packet_idx = 0;
        if (pkt_out) {
            pkt_out[0] = s_packet[0];
            pkt_out[1] = s_packet[1];
            pkt_out[2] = s_packet[2];
            return 1;
        }
    }
    return 0;
}

// Decode a completed packet + wake waiters.  Called OUTSIDE the i8042
// lock under the caller's preempt guard (see i8042_isr_drain): the decode
// fans out to the evt ring and evdev (both wake waiters), which must not
// run under the controller lock or trip a mid-ISR context switch.
void mouse_isr_packet(const uint8_t* pkt) {
    packet_decode(pkt);
    wait_queue_wake_all(&g_mouse_waitq);
}

// ── IRQ12 handler — runs in interrupt context ─────────────────────────────

extern void irq12_entry(void);

void mouse_irq_handler(void) {
    // One shared consume path with the keyboard ISR — see input_core.c.
    i8042_isr_drain();
}

// ── Mouse driver thread ───────────────────────────────────────────────────
// Keeps IRQ12 unmasked by consuming irq_notify wakeups.
// The actual work is done in the ISR (packet accumulation + ring buffer push);
// this thread exists so irq_wait(12) can sleep instead of busy-waiting.

static void mouse_thread_fn(void) {
    for (;;) {
        irq_wait(12);
        // Nothing to do here — packet decoding happens in the ISR.
        // This thread just drains the irq_notify count so it doesn't
        // accumulate unboundedly.
    }
}

// ── Public API ────────────────────────────────────────────────────────────

int mouse_has_events(void) {
    return s_evt_head != s_evt_tail;
}

int mouse_read(mouse_event_t* buf, int max) {
    int n = 0;
    while (n < max && s_evt_head != s_evt_tail) {
        buf[n++] = s_evt_buf[s_evt_head];
        s_evt_head = (s_evt_head + 1) & (EVT_BUF_SIZE - 1);
    }
    return n;
}

void mouse_init(void) {
    wait_queue_init(&g_mouse_waitq);

    // 1. Flush any stale data in the KBC output buffer.
    kbc_flush();

    // 2. Enable the auxiliary PS/2 port.
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_ENABLE_AUX);

    // 3. Read the Controller Command Byte and enable IRQ12 + aux clock.
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_READ_CCB);
    kbc_wait_read();
    uint8_t ccb = inb(KBC_DATA);
    ccb |=  (1 << 1);   // enable IRQ12 (AUX IRQ enable bit)
    ccb &= ~(1 << 5);   // clear "disable aux clock" bit
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_WRITE_CCB);
    kbc_wait_write();
    outb(KBC_DATA, ccb);

    // 4. Reset the mouse and wait for ACK + self-test result (0xAA).
    mouse_write(MOUSE_CMD_RESET);
    mouse_read_byte();   // ACK (0xFA)
    mouse_read_byte();   // self-test result (0xAA)
    mouse_read_byte();   // mouse ID (0x00 for standard mouse)

    // 5. Set defaults (100 samples/s, 4 counts/mm, stream mode).
    mouse_write(MOUSE_CMD_SET_DEFAULTS);
    mouse_read_byte();   // ACK

    // 6. Enable data reporting (start sending packets).
    mouse_write(MOUSE_CMD_ENABLE_REPORT);
    mouse_read_byte();   // ACK

    // 7. Install real handler, unmask IRQ12, spawn thread.
    idt_irq_register(0x2C, (uint64_t)irq12_entry);
    ioapic_unmask(ioapic_isa_to_gsi(12));
    task_t* t = task_create_kthread(mouse_thread_fn, pid_alloc());
    if (t) sched_add(t);
}
