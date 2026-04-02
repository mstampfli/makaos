#include "tsc.h"
#include "common.h"

/* ── PIT channel 2 ports ─────────────────────────────────────────────────── */
#define PIT_CMD    0x43   /* mode/command register                           */
#define PIT_CH2    0x42   /* channel 2 data port                             */
#define PORT_61    0x61   /* PC speaker / PIT ch2 gate control               */
/*   bit 0: PIT ch2 gate (1 = counting enabled)                              */
/*   bit 1: speaker enable (we keep it 0)                                    */
/*   bit 5: PIT ch2 OUT line (1 = counter expired in mode 0)                 */

/* Calibration window: ~10 ms at the PIT's 1,193,182 Hz input clock. */
#define CALIB_TICKS 11932u   /* 1193182 / 100 ≈ 11931.82 → round up        */

/* ── Module state ─────────────────────────────────────────────────────────── */
static uint64_t s_tsc_hz   = 0;   /* measured TSC frequency in Hz            */
static uint64_t s_tsc_boot = 0;   /* TSC value at tsc_init() time            */

/* ── rdtsc helper ─────────────────────────────────────────────────────────── */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── tsc_init ─────────────────────────────────────────────────────────────── */
/*
 * Calibrate by timing PIT channel 2 in one-shot (mode 0) mode.
 * Channel 2 does NOT use interrupts — OUT goes high when the counter hits 0,
 * and bit 5 of port 0x61 reflects that line.  We spin-poll it, which is fine
 * because this runs once at boot before tasks start.
 *
 * We measure two TSC samples separated by exactly CALIB_TICKS PIT cycles
 * (~10 ms).  Multiplying by 100 gives Hz.
 */
void tsc_init(void) {
    /* Save port 0x61 state so we can restore it afterwards. */
    uint8_t p61_saved = inb(PORT_61);

    /* Disable the gate first (bit 0 = 0) so the counter doesn't start yet. */
    outb(PORT_61, p61_saved & ~0x03u);   /* gate off, speaker off */

    /* Program channel 2: lo/hi access, mode 0 (one-shot), binary counting.
     * Command byte: bits[7:6]=10 (ch2), bits[5:4]=11 (lo/hi), bits[3:1]=000 (mode 0). */
    outb(PIT_CMD, 0xB0u);
    outb(PIT_CH2, (uint8_t)(CALIB_TICKS & 0xFFu));
    outb(PIT_CH2, (uint8_t)(CALIB_TICKS >> 8));

    /* Latch the start TSC, then enable the gate to begin the countdown. */
    uint64_t t0 = rdtsc();
    outb(PORT_61, (p61_saved & ~0x02u) | 0x01u);   /* gate on, speaker off */

    /* Spin until OUT goes high (bit 5 of port 0x61). */
    while (!(inb(PORT_61) & 0x20u));

    uint64_t t1 = rdtsc();

    /* Restore port 0x61. */
    outb(PORT_61, p61_saved);

    /* TSC delta over ~10 ms → scale to 1 s. */
    uint64_t delta = t1 - t0;
    s_tsc_hz   = delta * 100u;   /* × 100 because we measured 1/100 of a second */
    s_tsc_boot = t1;             /* t1 is our "boot" reference point             */
}

/* ── tsc_read_ns ──────────────────────────────────────────────────────────── */
/*
 * Returns nanoseconds since tsc_init().
 *
 * Avoid overflow: (ticks × 1e9) / hz can overflow uint64_t when ticks is
 * large.  Use __uint128_t for the intermediate product — GCC emits a
 * software 128-bit divide only if needed; at typical TSC rates (≤ 4 GHz)
 * the quotient fits comfortably in uint64_t.
 */
uint64_t tsc_read_ns(void) {
    uint64_t ticks = rdtsc() - s_tsc_boot;
    uint64_t secs  = ticks / s_tsc_hz;
    uint64_t rem   = ticks % s_tsc_hz;          /* rem < s_tsc_hz             */
    /* rem * 1e9 < 18.4e18 for any tsc_hz ≤ ~18 GHz — fits in uint64_t.     */
    return secs * 1000000000ULL + rem * 1000000000ULL / s_tsc_hz;
}
