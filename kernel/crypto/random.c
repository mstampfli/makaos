// ── Kernel CSPRNG — multi-source entropy pool + ChaCha20 DRBG ─────
//
// Design:
//
//   Pool: 32-byte SHA-256 state (`g_pool`).  Represents the
//   accumulated entropy.  Mixing any input involves hashing
//   g_pool || new_input into a fresh 32 bytes that replaces g_pool.
//   Periodic re-seeds fold in RDRAND + TSC jitter.
//
//   Output: ChaCha20 keyed by a snapshot of g_pool; counter bumps
//   as callers consume bytes.  Each kcsprng_read starts from a
//   fresh block counter at 0 against a fresh 16-byte nonce drawn
//   from the pool — prevents key/counter reuse across calls.
//
//   IRQ mixing: every IRQ handler calls kcsprng_mix_irq(vec, tsc_ns).
//   To keep the hot path cheap and lock-free, we maintain a per-CPU
//   "fast-mix" 32-byte array that XORs the IRQ's bytes in place.
//   Once per N IRQs (or on explicit demand), the fast-mix contents
//   are folded into g_pool under a spinlock.  Low-cost on hot path;
//   accumulated entropy is never lost.

#include "random.h"
#include "sha256.h"
#include "chacha20.h"
#include "smp.h"
#include "cpu.h"

extern uint64_t tsc_read_ns(void);

// ── State ─────────────────────────────────────────────────────────

static uint8_t        g_pool[SHA256_DIGEST_SZ];    // main entropy state
static spinlock_t     g_pool_lock = SPINLOCK_INIT;
static volatile int   g_inited    = 0;

// Per-CPU "fast" entropy sink — XOR'd into on every IRQ, flushed
// into g_pool under lock periodically.  Size 64 bytes — one cache
// line, keeps the IRQ hot path from touching shared state.
typedef struct {
    uint8_t  bytes[64];
    uint32_t counter;       // bumped on each IRQ mix
} __attribute__((aligned(64))) fast_mix_t;

static fast_mix_t g_fastmix[MAX_CPUS];

// Flush threshold: every 256 IRQs per CPU, fold fastmix into pool.
#define FASTMIX_FLUSH_EVERY 256u

// ── Hardware RNG (RDRAND / RDSEED) ────────────────────────────────
// Not trusted — one input among many.  If the CPU lacks the
// instructions we fall through silently; the rest of the sources
// still produce an unpredictable pool.

static int s_has_rdrand = 0;
static int s_has_rdseed = 0;

static void detect_hwrng(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    s_has_rdrand = (ecx & (1u << 30)) != 0;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0));
    s_has_rdseed = (ebx & (1u << 18)) != 0;
}

static int rdrand64(uint64_t* out) {
    if (!s_has_rdrand) return 0;
    uint64_t v; uint8_t ok;
    // Retry a few times — RDRAND is allowed to fail transiently.
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1"
            : "=r"(v), "=q"(ok)
            :
            : "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

static int rdseed64(uint64_t* out) {
    if (!s_has_rdseed) return 0;
    uint64_t v; uint8_t ok;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdseed %0; setc %1"
            : "=r"(v), "=q"(ok)
            :
            : "cc");
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

// ── Pool mix: absorb arbitrary bytes into g_pool via SHA-256 ──────
// SHA-256(g_pool || new) → g_pool.  Preserves accumulated entropy
// (attacker would have to invert SHA-256 to recover pool state).

static void pool_mix_locked(const void* data, uint64_t len) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, g_pool, sizeof(g_pool));
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, g_pool);
}

void kcsprng_mix(const void* data, uint64_t len) {
    if (!len) return;
    uint64_t f = spin_lock_irqsave(&g_pool_lock);
    pool_mix_locked(data, len);
    spin_unlock_irqrestore(&g_pool_lock, f);
}

// ── IRQ-path fast mix (lockless, per-CPU) ──────────────────────────

void kcsprng_mix_irq(uint8_t vector, uint64_t tsc_ns) {
    // XOR { vector, tsc_ns } into the local fastmix slot.  Plain
    // stores — we own this CPU's slot; no atomic needed.
    //
    // Guard: if cpu_init_bsp hasn't programmed GS_BASE yet (very
    // early kmain boot), %gs:0 reads as NULL.  Drop the sample —
    // we still get entropy later once the kernel is fully up.
    cpu_t* c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    if (!c) return;
    uint32_t id = c->id;
    if (id >= MAX_CPUS) return;

    fast_mix_t* fm = &g_fastmix[id];
    uint8_t* dst = fm->bytes + (fm->counter * 9) % sizeof(fm->bytes);
    // XOR 8 bytes of tsc_ns at a strided position, plus the vector.
    for (int i = 0; i < 8; i++)
        dst[(i) % (sizeof(fm->bytes) - 1)] ^= (uint8_t)(tsc_ns >> (i * 8));
    dst[0] ^= vector;
    fm->counter++;

    // Periodic flush into the global pool.  Under lock so we don't
    // interleave with kcsprng_read's snapshot.
    if ((fm->counter & (FASTMIX_FLUSH_EVERY - 1)) == 0) {
        uint64_t f = spin_lock_irqsave(&g_pool_lock);
        pool_mix_locked(fm->bytes, sizeof(fm->bytes));
        // Zero the local slot — don't let the same bytes get mixed
        // twice.  (After zeroing, XOR'd inputs from this CPU are
        // still accumulating in fresh positions.)
        for (unsigned i = 0; i < sizeof(fm->bytes); i++) fm->bytes[i] = 0;
        spin_unlock_irqrestore(&g_pool_lock, f);
    }
}

// ── init: seed with whatever entropy is available at kmain ──────

void kcsprng_init(void) {
    if (g_inited) return;

    detect_hwrng();

    // Mix in 8 × RDRAND, 4 × RDSEED, TSC jitter samples, and the
    // .bss-adjacent uninitialised DRAM noise.
    uint64_t seed_bytes[32];
    uint32_t n = 0;
    for (int i = 0; i < 8 && n < 32; i++) {
        if (rdrand64(&seed_bytes[n])) n++;
    }
    for (int i = 0; i < 4 && n < 32; i++) {
        if (rdseed64(&seed_bytes[n])) n++;
    }
    // TSC jitter: sample 16 times with a short busy wait between.
    // The low bits of consecutive reads differ by chaotic amounts.
    uint64_t prev = tsc_read_ns();
    for (int i = 0; i < 16 && n < 32; i++) {
        for (volatile int j = 0; j < 100; j++) { }
        uint64_t now = tsc_read_ns();
        seed_bytes[n++] = now ^ (prev << 13);
        prev = now;
    }
    // Boot DRAM sample: read some bytes at a fixed address in kernel
    // space that we haven't cleared yet.  If kmain already zeroed
    // BSS this is just zeros, which contributes nothing but also
    // doesn't harm.
    extern char __kernel_end[];
    if (n < 32) {
        seed_bytes[n++] = *(volatile uint64_t*)((char*)__kernel_end + 4096);
    }

    uint64_t f = spin_lock_irqsave(&g_pool_lock);
    pool_mix_locked(seed_bytes, n * sizeof(uint64_t));
    spin_unlock_irqrestore(&g_pool_lock, f);

    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
}

// ── output: ChaCha20 DRBG ─────────────────────────────────────────
//
// Each call snapshots the pool into a fresh (key, nonce).  Counter
// starts at 0 for this call; output is the ChaCha20 keystream.
// We also mix one block of output back into the pool so consecutive
// calls don't return the same sequence even if no new entropy came
// in between.

void kcsprng_read(void* out, uint64_t len) {
    if (!out || !len) return;
    // If not inited yet, init on demand.  Any pre-init caller gets
    // low-entropy output (boot-time DRAM + whatever RDRAND we can
    // get in this context) — better than the xorshift we had before.
    if (!__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) kcsprng_init();

    uint8_t key[32];
    uint8_t nonce[12];

    uint64_t f = spin_lock_irqsave(&g_pool_lock);
    // Mix in any stale tsc_read_ns so two consecutive calls with no
    // intervening IRQs still diverge.
    uint64_t t = tsc_read_ns();
    pool_mix_locked(&t, sizeof(t));

    // Snapshot: first 32 bytes of pool → ChaCha20 key.  Re-hash
    // pool into fresh state + last 12 bytes → nonce.
    for (int i = 0; i < 32; i++) key[i] = g_pool[i];

    // Re-hash to derive a separate nonce without key/nonce overlap.
    sha256_ctx_t nctx;
    sha256_init(&nctx);
    sha256_update(&nctx, g_pool, sizeof(g_pool));
    sha256_update(&nctx, "nonce", 5);
    uint8_t nbuf[32];
    sha256_final(&nctx, nbuf);
    for (int i = 0; i < 12; i++) nonce[i] = nbuf[i];

    // Advance pool state so next call won't produce the same
    // key/nonce pair even under no entropy influx.
    sha256_init(&nctx);
    sha256_update(&nctx, g_pool, sizeof(g_pool));
    sha256_update(&nctx, "advance", 7);
    sha256_final(&nctx, g_pool);
    spin_unlock_irqrestore(&g_pool_lock, f);

    // Stream output bytes via ChaCha20.  XOR against zero buffer to
    // produce raw keystream (use chacha20_xor with in == all-zero).
    uint8_t* p = (uint8_t*)out;
    uint8_t  zero_blk[CHACHA20_BLOCK_SZ];
    for (int i = 0; i < CHACHA20_BLOCK_SZ; i++) zero_blk[i] = 0;
    uint32_t ctr = 0;
    while (len) {
        uint32_t take = (len < CHACHA20_BLOCK_SZ) ? (uint32_t)len : CHACHA20_BLOCK_SZ;
        chacha20_xor(p, zero_blk, take, key, nonce, ctr++);
        p += take; len -= take;
    }

    // Zero ephemeral material.
    for (int i = 0; i < 32; i++) key[i] = 0;
    for (int i = 0; i < 12; i++) nonce[i] = 0;
}
