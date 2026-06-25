// ── futex.c — sleeping lock back-end for userland threading ─────────
//
// Before this existed, libc's pthread_mutex/cond/semaphore spun with
// sched_yield.  Under SMP that burned whole cores (sway's GLib worker
// pinned a CPU for its lifetime) and starved the very threads the
// spinners were waiting on — foot's render workers and sway's client
// handling both stalled this way.
//
// Design: a fixed hash of buckets, each a spinlock + singly-linked
// list of stack-resident waiters.  Wakers unlink the waiters they
// wake (under the bucket lock), so a woken waiter never touches the
// list again; a timed-out/interrupted waiter unlinks itself.  The
// lost-wake race is closed the same way Linux does it: the waiter
// enqueues FIRST, then re-reads the user word — any unlock that
// happens after the enqueue must observe the waiter when it calls
// FUTEX_WAKE, and any unlock before it is caught by the re-read.

#include "futex.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "cpu.h"
#include "errno.h"

extern uint64_t tsc_read_ns(void);
extern int copy_from_user(void* dst, const void* src_u, uint64_t len);

#define FUTEX_BUCKETS 256

typedef struct futex_waiter {
    struct futex_waiter* next;
    task_t*   task;
    void*     mm;       // address-space identity (process-private key)
    uint64_t  uaddr;
    volatile int woken;
} futex_waiter_t;

typedef struct {
    spinlock_t      lock;
    futex_waiter_t* head;
} futex_bucket_t;

static futex_bucket_t s_buckets[FUTEX_BUCKETS];

static inline futex_bucket_t* bucket_of(void* mm, uint64_t uaddr) {
    uint64_t h = ((uint64_t)mm >> 4) ^ (uaddr >> 2);
    h ^= h >> 16;
    h ^= h >> 8;
    return &s_buckets[h & (FUTEX_BUCKETS - 1)];
}

int64_t futex_wait(uint32_t* uaddr, uint32_t val, uint64_t timeout_ns) {
    if (!g_current || !uaddr) return -EINVAL;
    if (((uint64_t)uaddr & 3) != 0) return -EINVAL;

    void* mm = g_current->mm_shared->mm;
    futex_bucket_t* b = bucket_of(mm, (uint64_t)uaddr);

    // Fast reject of a bad/unmapped pointer before we enqueue.  copy_from_user
    // validates (_access_ok) + prefaults; it is fault-safe (returns -EFAULT,
    // never a kernel #PF) and is NOT under any lock here.
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return -EFAULT;
    if (cur != val) return -EAGAIN;   // already changed: no need to enqueue

    futex_waiter_t w = {
        .next  = (futex_waiter_t*)0,
        .task  = g_current,
        .mm    = mm,
        .uaddr = (uint64_t)uaddr,
        .woken = 0,
    };

    spin_lock(&b->lock);
    w.next  = b->head;
    b->head = &w;
    spin_unlock(&b->lock);

    // Authoritative re-read AFTER publishing the enqueue, done with a fault-safe
    // copy_from_user OUTSIDE b->lock.  The old code re-read with a raw
    // `*(volatile uint32_t*)uaddr` while HOLDING b->lock: a concurrent
    // munmap/mprotect(PROT_NONE) of the page (another thread of this process,
    // unserialised against the futex bucket) faulted that read -> the #PF
    // handler takes mm->vma_lock and may alloc/sleep while a raw spinlock is
    // held (fault-under-spinlock + lock nest -> panic/DoS).  Doing it via
    // copy_from_user with the lock dropped returns -EFAULT instead.
    //
    // Lost-wake fence still holds: the spin_lock acquire above orders this read
    // after any waker's prior bucket-unlock, so a concurrent FUTEX_WAKE either
    // already observed our enqueue (and unlinked + woke us -> w.woken) or wrote
    // the new value that we now read (cur != val).
    uint32_t cur2;
    int fault = copy_from_user(&cur2, uaddr, sizeof(cur2));
    if (fault != 0 || cur2 != val) {
        spin_lock(&b->lock);
        if (w.woken) {
            // A waker unlinked + woke us between the enqueue and this re-read;
            // consume the wake as a successful wait.
            spin_unlock(&b->lock);
            return 0;
        }
        for (futex_waiter_t** pp = &b->head; *pp; pp = &(*pp)->next) {
            if (*pp == &w) { *pp = w.next; break; }
        }
        spin_unlock(&b->lock);
        return fault ? -EFAULT : -EAGAIN;
    }

    uint64_t deadline = timeout_ns ? tsc_read_ns() + timeout_ns : 0;
    int64_t rc = 0;

    while (!__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
        if (signal_has_actionable(&g_current->sigstate)) { rc = -EINTR; break; }
        if (deadline) {
            uint64_t now = tsc_read_ns();
            if (now >= deadline) { rc = -ETIMEDOUT; break; }
            g_current->sleep_until_ns = deadline;
        }
        // sched_wake (from futex_wake) flags wake_pending, so a wake
        // landing between the check above and this sleep makes
        // sched_sleep return immediately — no lost wake.
        sched_sleep();
        g_current->sleep_until_ns = 0;
    }
    g_current->sleep_until_ns = 0;

    if (rc != 0) {
        // Timed out / interrupted: unlink ourselves — unless a waker
        // got here first (it unlinked us and is about to (or already
        // did) sched_wake us); that counts as a successful wake.
        spin_lock(&b->lock);
        if (!w.woken) {
            for (futex_waiter_t** pp = &b->head; *pp; pp = &(*pp)->next) {
                if (*pp == &w) { *pp = w.next; break; }
            }
        } else {
            rc = 0;
        }
        spin_unlock(&b->lock);
    }
    return rc;
}

int64_t futex_wake(uint32_t* uaddr, uint32_t nwake) {
    if (!g_current || !uaddr) return -EINVAL;
    if (nwake == 0) return 0;

    void* mm = g_current->mm_shared->mm;
    futex_bucket_t* b = bucket_of(mm, (uint64_t)uaddr);

    int64_t count = 0;
    spin_lock(&b->lock);
    futex_waiter_t** pp = &b->head;
    while (*pp && (uint64_t)count < nwake) {
        futex_waiter_t* w = *pp;
        if (w->mm == mm && w->uaddr == (uint64_t)uaddr) {
            *pp = w->next;                                  // unlink first
            __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            sched_wake(w->task);
            count++;
        } else {
            pp = &w->next;
        }
    }
    spin_unlock(&b->lock);
    return count;
}
