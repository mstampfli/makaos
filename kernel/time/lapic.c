#include "lapic.h"
#include "tsc.h"
#include "idt.h"
#include "common.h"

// ── x2APIC MSR addresses ────────────────────────────────────────────────
// Every legacy xAPIC register at MMIO offset `off` maps to MSR
// `0x800 + (off >> 4)` in x2APIC mode.  The exceptions: ICR is now a
// single 64-bit MSR (0x830) instead of two 32-bit halves; SELF IPI is
// a new register at 0x83F.

#define MSR_IA32_APIC_BASE   0x0000001Bu   // [11]=EN, [10]=EXTD (x2APIC)
#define APIC_BASE_EN         (1u << 11)
#define APIC_BASE_EXTD       (1u << 10)

#define X2APIC_MSR_BASE      0x800u

#define X2APIC_ID            (X2APIC_MSR_BASE + 0x02u)   // 0x802 — read-only 32-bit APIC ID
#define X2APIC_VERSION       (X2APIC_MSR_BASE + 0x03u)   // 0x803
#define X2APIC_TPR           (X2APIC_MSR_BASE + 0x08u)   // 0x808
#define X2APIC_EOI           (X2APIC_MSR_BASE + 0x0Bu)   // 0x80B — write 0 to ack
#define X2APIC_SVR           (X2APIC_MSR_BASE + 0x0Fu)   // 0x80F — spurious vector + enable
#define X2APIC_ESR           (X2APIC_MSR_BASE + 0x28u)   // 0x828 — error status
#define X2APIC_ICR           (X2APIC_MSR_BASE + 0x30u)   // 0x830 — **64-bit** command, atomic
#define X2APIC_LVT_TIMER     (X2APIC_MSR_BASE + 0x32u)   // 0x832
#define X2APIC_LVT_LINT0     (X2APIC_MSR_BASE + 0x35u)   // 0x835
#define X2APIC_LVT_LINT1     (X2APIC_MSR_BASE + 0x36u)   // 0x836
#define X2APIC_LVT_ERROR     (X2APIC_MSR_BASE + 0x37u)   // 0x837
#define X2APIC_TIMER_INIT    (X2APIC_MSR_BASE + 0x38u)   // 0x838
#define X2APIC_TIMER_CUR     (X2APIC_MSR_BASE + 0x39u)   // 0x839 — read-only
#define X2APIC_TIMER_DCR     (X2APIC_MSR_BASE + 0x3Eu)   // 0x83E
#define X2APIC_SELF_IPI      (X2APIC_MSR_BASE + 0x3Fu)   // 0x83F — x2APIC only

// SVR bits
#define SVR_ENABLE           (1u << 8)

// LVT bits
#define LVT_MASKED           (1u << 16)
#define LVT_PERIODIC         (1u << 17)

// Timer DCR
#define DCR_DIVIDE_BY_1      0x0Bu

// ICR fields — same bit layout as the legacy low dword of xAPIC ICR,
// just packed into the low 32 bits of the 64-bit MSR value.  The
// high 32 bits hold the full 32-bit destination APIC ID.
#define ICR_VECTOR(v)        ((uint64_t)(uint8_t)(v))
#define ICR_DELIVERY_FIXED   (0uLL << 8)
#define ICR_DELIVERY_INIT    (5uLL << 8)
#define ICR_DELIVERY_STARTUP (6uLL << 8)
#define ICR_DEST_PHYSICAL    (0uLL << 11)
#define ICR_ASSERT           (1uLL << 14)
#define ICR_LEVEL_EDGE       (0uLL << 15)
#define ICR_DEST_SELF        (1uLL << 18)

// ── Module state ────────────────────────────────────────────────────────
// Single number across all CPUs — the LAPIC timer frequency per ms,
// measured once on the BSP during lapic_init().  APs inherit it.
static uint32_t s_timer_ticks_per_ms = 0;

// ── Low-level MSR helpers ───────────────────────────────────────────────
// These must be ALWAYS_INLINE so every lapic access compiles to the raw
// `wrmsr` / `rdmsr` pair with no call overhead.  x2APIC register access
// is supposed to be a single instruction, not a function call.

ALWAYS_INLINE static void wrmsr_u32(uint32_t msr, uint32_t val) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(val), "d"(0u)
                     : "memory");
}

ALWAYS_INLINE static void wrmsr_u64(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(lo), "d"(hi)
                     : "memory");
}

ALWAYS_INLINE static uint32_t rdmsr_u32(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    (void)hi;
    return lo;
}

ALWAYS_INLINE static uint64_t rdmsr_u64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ── CPUID helper ────────────────────────────────────────────────────────

ALWAYS_INLINE static void cpuid(uint32_t leaf,
                                  uint32_t* eax, uint32_t* ebx,
                                  uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

// ── Public API ──────────────────────────────────────────────────────────

void lapic_eoi(void) {
    // x2APIC EOI: single wrmsr of 0 to MSR 0x80B.  No serialising read
    // needed afterwards — x2APIC writes are ordered with subsequent
    // interrupt delivery by the CPU itself.
    wrmsr_u32(X2APIC_EOI, 0);
}

uint32_t lapic_id(void) {
    // x2APIC ID register is a full 32-bit value — no shift-and-mask
    // like the xAPIC case where the ID lives in bits [27:24].
    return rdmsr_u32(X2APIC_ID);
}

uint64_t lapic_msi_addr(void) {
    // MSI still uses the 0xFEE00000 compatibility address format
    // upstream, because the PCI fabric and IOAPIC are not x2APIC-
    // aware.  The 8-bit destination field in bits [19:12] limits
    // MSI destination routing to APIC IDs 0–255, which is fine for
    // the kernel's MAX_CPUS=64.  For larger systems, interrupt
    // remapping (VT-d) is the right answer.
    return 0xFEE00000ULL | ((uint64_t)(lapic_id() & 0xFFu) << 12);
}

uint32_t lapic_msi_data(uint8_t vector) {
    return (uint32_t)vector;
}

// ── Timer calibration ───────────────────────────────────────────────────
// Measure LAPIC timer ticks per ms against the TSC.  The LAPIC timer's
// input clock is the CPU's bus clock divided by the DCR, so it's
// independent of TSC frequency — we have to sample it empirically.

static void calibrate_timer(void) {
    wrmsr_u32(X2APIC_TIMER_DCR, DCR_DIVIDE_BY_1);
    wrmsr_u32(X2APIC_LVT_TIMER, LVT_MASKED | VEC_LAPIC_TIMER);
    wrmsr_u32(X2APIC_TIMER_INIT, 0xFFFFFFFFu);   // start counting down from max

    uint64_t t0 = tsc_read_ns();
    while (tsc_read_ns() - t0 < 10000000ULL) { /* busy-wait exactly 10 ms */ }

    uint32_t cur   = rdmsr_u32(X2APIC_TIMER_CUR);
    uint32_t delta = 0xFFFFFFFFu - cur;        // ticks elapsed in ~10 ms
    s_timer_ticks_per_ms = delta / 10u;

    wrmsr_u32(X2APIC_TIMER_INIT, 0);            // stop counting
}

// Enable x2APIC mode on the current CPU and bring the LAPIC to a
// known-good state.  Called by both lapic_init (BSP) and lapic_init_ap
// (APs) via this shared helper.
static void enable_x2apic_and_reset(void) {
    // 1. Probe x2APIC support.  CPUID.01h ECX bit 21.
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 21))) {
        serial_puts_dbg("[lapic] PANIC: CPU does not support x2APIC\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    // 2. Flip IA32_APIC_BASE to enable x2APIC.  Writing EXTD without EN
    //    is a #GP, and toggling EXTD while EN is set without going
    //    through a disable is also unsafe.  We first make sure EN is
    //    set (the firmware/bootloader should have left it on, but be
    //    defensive), then OR in EXTD.
    uint64_t base = rdmsr_u64(MSR_IA32_APIC_BASE);
    base |= APIC_BASE_EN;          // ensure legacy xAPIC enable
    base |= APIC_BASE_EXTD;         // switch to x2APIC
    wrmsr_u64(MSR_IA32_APIC_BASE, base);

    // From here on, every MMIO access to 0xFEE00000 would #GP.
    // We exclusively use MSR access below.

    // 3. TPR = 0 so every interrupt vector is accepted.
    wrmsr_u32(X2APIC_TPR, 0);

    // 4. Mask the LINT lines (we don't use NMI/extint via the LAPIC).
    wrmsr_u32(X2APIC_LVT_LINT0, LVT_MASKED);
    wrmsr_u32(X2APIC_LVT_LINT1, LVT_MASKED);
    wrmsr_u32(X2APIC_LVT_ERROR, LVT_MASKED);

    // 5. Clear any stale error status.
    wrmsr_u32(X2APIC_ESR, 0);

    // 6. Enable the LAPIC via the spurious vector register.
    wrmsr_u32(X2APIC_SVR, VEC_LAPIC_SPURIOUS | SVR_ENABLE);
}

void lapic_init(uint64_t lapic_phys) {
    (void)lapic_phys;   // ignored: x2APIC has no MMIO base
    enable_x2apic_and_reset();
    calibrate_timer();
}

void lapic_init_ap(void) {
    enable_x2apic_and_reset();
    // Timer calibration is done once on the BSP; APs inherit
    // s_timer_ticks_per_ms via shared memory.  lapic_timer_start()
    // will use it when/if the AP starts its own periodic timer.
}

void lapic_timer_start(uint32_t hz) {
    uint32_t init = s_timer_ticks_per_ms * 1000u / hz;
    wrmsr_u32(X2APIC_TIMER_DCR, DCR_DIVIDE_BY_1);
    wrmsr_u32(X2APIC_LVT_TIMER, LVT_PERIODIC | VEC_LAPIC_TIMER);
    wrmsr_u32(X2APIC_TIMER_INIT, init);
}

// ── IPI send path ───────────────────────────────────────────────────────
//
// x2APIC's killer feature: ICR is a single 64-bit MSR (0x830).  A single
// `wrmsr` dispatches the IPI atomically — no Delivery Status polling
// loop before writing the low half like legacy xAPIC needed, because
// the hardware itself serialises back-to-back wrmsrs to the ICR.
//
// The 64-bit value layout (reconstructed from the xAPIC two-halves):
//   bits [ 7: 0] vector
//   bits [10: 8] delivery mode (0=fixed, 5=INIT, 6=SIPI)
//   bit  [11]    destination mode (0=physical, 1=logical)
//   bits [13:12] delivery status (RO in x2APIC — ignored on write)
//   bit  [14]    level (1=assert)
//   bit  [15]    trigger mode (0=edge, 1=level)
//   bits [19:18] destination shorthand (0=none, 1=self, 2=all, 3=all-except-self)
//   bits [63:32] destination APIC ID (full 32 bits in x2APIC)

static inline void icr_write(uint64_t value) {
    wrmsr_u64(X2APIC_ICR, value);
}

void lapic_send_ipi(uint32_t target_apic_id, uint8_t vector) {
    icr_write(((uint64_t)target_apic_id << 32)
              | ICR_VECTOR(vector)
              | ICR_DELIVERY_FIXED
              | ICR_DEST_PHYSICAL
              | ICR_ASSERT
              | ICR_LEVEL_EDGE);
}

void lapic_send_init(uint32_t target_apic_id) {
    icr_write(((uint64_t)target_apic_id << 32)
              | ICR_DELIVERY_INIT
              | ICR_DEST_PHYSICAL
              | ICR_ASSERT
              | ICR_LEVEL_EDGE);
}

void lapic_send_sipi(uint32_t target_apic_id, uint8_t vector) {
    icr_write(((uint64_t)target_apic_id << 32)
              | ICR_VECTOR(vector)
              | ICR_DELIVERY_STARTUP
              | ICR_DEST_PHYSICAL
              | ICR_ASSERT
              | ICR_LEVEL_EDGE);
}
