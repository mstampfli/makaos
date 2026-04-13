#include "lapic.h"
#include "vmm.h"
#include "tsc.h"
#include "idt.h"
#include "common.h"

// ── xAPIC MMIO register offsets ───────────────────────────────────────────
// All registers are 32-bit, 16-byte aligned.

#define LAPIC_ID          0x020u   // LAPIC ID register (RO)
#define LAPIC_VER         0x030u   // LAPIC version (RO)
#define LAPIC_TPR         0x080u   // Task Priority Register — set to 0 (accept all)
#define LAPIC_EOI         0x0B0u   // End-of-Interrupt (write 0 to acknowledge)
#define LAPIC_SPURIOUS    0x0F0u   // Spurious Interrupt Vector Register (SVR)
#define LAPIC_ICR_LO      0x300u   // Interrupt Command Register low  (write triggers IPI)
#define LAPIC_ICR_HI      0x310u   // Interrupt Command Register high (target LAPIC ID)
#define LAPIC_TIMER_LVT   0x320u   // LVT Timer entry
#define LAPIC_TIMER_INIT  0x380u   // Timer Initial Count
#define LAPIC_TIMER_CUR   0x390u   // Timer Current Count (RO)
#define LAPIC_TIMER_DCR   0x3E0u   // Timer Divide Configuration Register

// SVR bits
#define SVR_ENABLE        (1u << 8)   // LAPIC software enable

// LVT timer bits
#define LVT_MASKED        (1u << 16)
#define LVT_PERIODIC      (1u << 17)  // bit 17 set → periodic mode

// Timer divide values for DCR
#define DCR_DIVIDE_BY_1   0x0Bu   // divide by 1 (fastest count rate)

// ── Module state ──────────────────────────────────────────────────────────

static volatile uint32_t* s_lapic = NULL;  // kernel virtual base
static uint64_t           s_phys  = 0;     // physical base (for MSI address)
static uint32_t           s_timer_ticks_per_hz = 0; // LAPIC ticks in one scheduler period

// ── MMIO helpers ──────────────────────────────────────────────────────────

static inline uint32_t lapic_r(uint32_t off) {
    return s_lapic[off >> 2];
}
static inline void lapic_w(uint32_t off, uint32_t val) {
    s_lapic[off >> 2] = val;
}

// ── Public API ────────────────────────────────────────────────────────────

void lapic_eoi(void) {
    lapic_w(LAPIC_EOI, 0);
}

uint8_t lapic_id(void) {
    return (uint8_t)(lapic_r(LAPIC_ID) >> 24);
}

uint64_t lapic_msi_addr(void) {
    // MSI address: 0xFEE[LAPIC_ID]00000.  For BSP (ID=0): 0xFEE00000.
    // Bits [19:12] encode the destination LAPIC ID in xAPIC mode.
    return 0xFEE00000ULL | ((uint64_t)lapic_id() << 12);
}

uint32_t lapic_msi_data(uint8_t vector) {
    // Delivery mode 000 = Fixed, level = 0, trigger = 0 (edge).
    // Bits [7:0] = vector, bits [10:8] = delivery mode (000), rest = 0.
    return (uint32_t)vector;
}

// ── LAPIC timer calibration ───────────────────────────────────────────────
// Strategy: measure how many LAPIC timer ticks occur in one millisecond by
// using the TSC (already calibrated by tsc_init()).  No spin-polling of PIT.
//
// 1. Set DCR to divide-by-1 (highest resolution).
// 2. Write a large initial count so the timer runs freely.
// 3. Record TSC and LAPIC current count.
// 4. Busy-wait 10 ms (via tsc_read_ns).
// 5. Read LAPIC current count again; delta = ticks in 10 ms.
// 6. Compute ticks_per_ms = delta / 10.

static void calibrate_timer(void) {
    lapic_w(LAPIC_TIMER_DCR, DCR_DIVIDE_BY_1);
    lapic_w(LAPIC_TIMER_LVT, LVT_MASKED | VEC_LAPIC_TIMER);  // masked during calib
    lapic_w(LAPIC_TIMER_INIT, 0xFFFFFFFFu);  // start counting down from max

    uint64_t t0 = tsc_read_ns();
    // Busy-wait exactly 10 ms.
    while (tsc_read_ns() - t0 < 10000000ULL);

    uint32_t cur = lapic_r(LAPIC_TIMER_CUR);
    uint32_t delta = 0xFFFFFFFFu - cur;  // ticks elapsed in ~10 ms

    // ticks_per_hz = ticks_per_10ms * 10ms / (1000ms/hz)
    // We store ticks-per-millisecond and multiply later.
    // delta / 10 = ticks per ms.  Store as ticks-per-ms * 1000 = ticks-per-s.
    // We compute ticks_per_hz on demand in lapic_timer_start.
    s_timer_ticks_per_hz = delta / 10u;  // ticks per ms (not yet divided by hz)

    // Stop the free-running timer (we start it properly in lapic_timer_start).
    lapic_w(LAPIC_TIMER_INIT, 0);
}

void lapic_init(uint64_t lapic_phys) {
    s_phys  = lapic_phys ? lapic_phys : 0xFEE00000ULL;
    s_lapic = (volatile uint32_t*)vmm_map_mmio((phys_addr_t)s_phys, 0x1000u);
    lapic_w(LAPIC_TPR, 0);
    lapic_w(LAPIC_SPURIOUS, VEC_LAPIC_SPURIOUS | SVR_ENABLE);
    calibrate_timer();
}

void lapic_timer_start(uint32_t hz) {
    uint32_t init = s_timer_ticks_per_hz * 1000u / hz;
    lapic_w(LAPIC_TIMER_DCR, DCR_DIVIDE_BY_1);
    lapic_w(LAPIC_TIMER_LVT, LVT_PERIODIC | VEC_LAPIC_TIMER);
    lapic_w(LAPIC_TIMER_INIT, init);
}
