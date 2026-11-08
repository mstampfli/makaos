// linux/futex.h - futex operation constants (Linux-compatible).
//
// MakaOS's futex is always process-private and uses a monotonic timeout, so the
// FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME bits are stripped by the syscall()
// shim (see libc.c) before the op reaches the kernel.

#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H 1

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_FD              2
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10

#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

#endif /* linux/futex.h */
