#pragma once
#include "common.h"
#include "smp.h"

// ── Sequence locks — readers never block ────────────────────────────────
//
// A seqlock is a reader/writer synchronization primitive where:
//   - Readers take NO lock.  They snapshot a sequence counter before
//     and after reading the data; if the values match and are even,
//     the read was consistent.  Otherwise, retry.
//   - Writers take a (short) write lock, bump the counter before and
//     after the write.
//
// Usage pattern:
//
//   // Reader:
//   uint32_t seq;
//   do {
//       seq = seq_begin(&sl);
//       // read fields...
//   } while (seq_retry(&sl, seq));
//
//   // Writer:
//   seq_write_begin(&sl);
//   // update fields...
//   seq_write_end(&sl);
//
// Readers spin only if a writer is actively updating.  They don't block
// each other at all.  Writers block each other via an external spinlock
// (optional; single-writer patterns skip it).
//
// WARNINGS:
//   - Readers MUST re-check seq before using any value from the read.
//     Values read during an active write may be torn/inconsistent —
//     that's fine as long as they're only used inside the retry loop.
//   - Reader loops must be finite.  Don't do blocking I/O, don't call
//     kmalloc, don't trigger page faults in the read section.  Copy
//     data out first, then process it.
//   - On ARM and other weakly-ordered architectures, seq_begin needs
//     an acquire fence and seq_retry needs a load_acquire.  x86's
//     strong ordering makes both compiler-only today.
//
// ── Multi-writer seqlock ─────────────────────────────────────────────────
//
// If multiple writers exist, use seq_write_lock / seq_write_unlock which
// take an embedded spinlock.  For single-writer cases (ext2 bcache slots
// owned by one I/O thread, clock written only by NTP adjuster), the
// spinlock is unused and wasted BSS — use seq_write_begin_unlocked /
// seq_write_end_unlocked instead.

typedef struct {
    volatile uint32_t seq;
    spinlock_t        write_lock;  // unused in single-writer mode
} seqlock_t;

#define SEQLOCK_INIT { 0, SPINLOCK_INIT }

ALWAYS_INLINE void seqlock_init(seqlock_t* sl) {
    atomic_store_relaxed(&sl->seq, 0u);
    spin_lock_init(&sl->write_lock);
}

// ── Reader API ───────────────────────────────────────────────────────────

// Begin a read section.  Returns the current sequence number.  If the
// value is odd, a writer is active — spin until it's even.
ALWAYS_INLINE uint32_t seq_begin(const seqlock_t* sl) {
    uint32_t s;
    for (;;) {
        s = atomic_load_acq(&sl->seq);
        if (LIKELY((s & 1u) == 0)) return s;
        cpu_relax();
    }
}

// After reading, call seq_retry with the value returned by seq_begin.
// If it returns non-zero, the read was inconsistent and must be retried.
ALWAYS_INLINE int seq_retry(const seqlock_t* sl, uint32_t start) {
    // Load-acquire ensures the read-side loads can't be reordered past
    // this check.  On x86 it's a compiler barrier.
    smp_rmb();
    return atomic_load_acq(&sl->seq) != start;
}

// ── Single-writer API (no internal lock) ────────────────────────────────

ALWAYS_INLINE void seq_write_begin_unlocked(seqlock_t* sl) {
    // Bump to odd — signals readers that a write is in progress.
    uint32_t s = atomic_load_relaxed(&sl->seq);
    atomic_store_rel(&sl->seq, s + 1u);
    // The release ensures subsequent stores (the actual write) happen
    // after the sequence bump.  Otherwise readers could see the new
    // data with the old sequence — a silent corruption.
    smp_wmb();
}

ALWAYS_INLINE void seq_write_end_unlocked(seqlock_t* sl) {
    // Bump to even — signals readers that the write is complete.
    smp_wmb();  // ensure data stores retire before the sequence bump
    uint32_t s = atomic_load_relaxed(&sl->seq);
    atomic_store_rel(&sl->seq, s + 1u);
}

// ── Multi-writer API (takes the internal spinlock) ──────────────────────

ALWAYS_INLINE void seq_write_begin(seqlock_t* sl) {
    spin_lock(&sl->write_lock);
    seq_write_begin_unlocked(sl);
}

ALWAYS_INLINE void seq_write_end(seqlock_t* sl) {
    seq_write_end_unlocked(sl);
    spin_unlock(&sl->write_lock);
}
