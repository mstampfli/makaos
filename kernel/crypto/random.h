#pragma once
#include "common.h"

// ── Kernel CSPRNG (Phase 9A) ──────────────────────────────────────
//
// Multi-source entropy pool + ChaCha20 DRBG at the output.
// Sources (mixed via SHA-256):
//   - RDRAND / RDSEED  (hardware RNG — contributing, not trusted)
//   - RDTSC jitter     (sampled on every IRQ; low bits unpredictable)
//   - IRQ timings      (vector + delta-TSC per IRQ)
//   - Boot-time DRAM   (uninitialised memory at BSS ends)
//
// No single source is trusted — output stays unpredictable as long
// as ANY one contributed true entropy.  Matches Linux's
// drivers/char/random.c design intent.  Replaces the previous
// xorshift64 /dev/urandom implementation.
//
// ── API ───────────────────────────────────────────────────────────

// Initialise at kmain (after RDTSC / RDRAND detection, before any
// consumer).  Pre-seeds from RDRAND × 8, boot-DRAM noise, initial
// TSC reading.
void kcsprng_init(void);

// Fill `out` with `len` cryptographically-random bytes.  Safe to call
// from any context (including IRQ — takes a spinlock).
void kcsprng_read(void* out, uint64_t len);

// Mix a small amount of fresh entropy into the pool.  Called from
// the IRQ dispatcher once per IRQ — tsc delta + vector number.
// Lockless (per-CPU staging + periodic pool update).
void kcsprng_mix_irq(uint8_t vector, uint64_t tsc_ns);

// Mix arbitrary data into the pool (network packet timings, disk
// completion latencies, etc).  Takes the pool lock.
void kcsprng_mix(const void* data, uint64_t len);
