#pragma once
#include "common.h"

// ── Futex — fast userspace mutex back-end ────────────────────────────
//
// The minimal Linux-style contract pthread needs:
//
//   FUTEX_WAIT: if *uaddr still equals `val`, sleep until a
//               FUTEX_WAKE on the same address (same address space),
//               a timeout, or a signal.
//   FUTEX_WAKE: wake up to `val` waiters on uaddr.
//
// Keys are process-private (mm pointer + user address) — exactly what
// pthread mutex/cond/semaphore need.  Process-shared futexes (shmem-
// backed keys) can be added when something wants them.
//
// Timeouts are RELATIVE nanoseconds (0 = wait forever).  This is the
// kernel half of the ABI; userland's pthread passes nanoseconds
// directly instead of a timespec pointer.

#define FUTEX_OP_WAIT 0
#define FUTEX_OP_WAKE 1
// Linux flag bits accepted and ignored (we are always "private", and
// our clock is the single kernel ns clock):
#define FUTEX_FLAG_PRIVATE        128
#define FUTEX_FLAG_CLOCK_REALTIME 256

// Returns 0 on wake, -EAGAIN if *uaddr != val at queue time,
// -ETIMEDOUT, -EINTR, -EFAULT.
int64_t futex_wait(uint32_t* uaddr, uint32_t val, uint64_t timeout_ns);

// Returns the number of waiters woken.
int64_t futex_wake(uint32_t* uaddr, uint32_t nwake);
