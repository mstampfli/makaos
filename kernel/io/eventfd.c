// ── eventfd — Linux-compatible 64-bit counter signalling fd ───────────
//
// A minimal, correct eventfd.  Every kernel-side write adds to the
// counter; reads drain (or decrement, in semaphore mode).  Waiters
// block via wait_queue, so no spin-polling anywhere.
//
// Hot path is lock-free: the counter is a single uint64_t atomic that
// readers/writers CAS against.  The wait_queue spinlock only matters
// when there's an actual blocker to wake.

#include "common.h"
#include "kheap.h"
#include "wait.h"
#include "smp.h"
#include "sched.h"
#include "process.h"
#include "vfs.h"
#include "syscall.h"
#include "errno.h"

// POLLIN/POLLOUT come from poll.h — pull the kernel copy through syscall.h.

typedef struct {
    uint64_t      count;        // atomic
    uint32_t      flags;        // EFD_SEMAPHORE | EFD_NONBLOCK
    wait_queue_t  read_wq;      // readers waiting for count > 0
    wait_queue_t  write_wq;     // writers waiting for count < MAX
} eventfd_state_t;

#define EVENTFD_MAX ((uint64_t)0xfffffffffffffffeULL)

// ── vfs_file_t operations ─────────────────────────────────────────────

static int64_t eventfd_read_op(vfs_file_t* self, void* buf, uint64_t len) {
    if (len < 8) return -EINVAL;
    eventfd_state_t* s = (eventfd_state_t*)self->ctx;
    const int sema     = !!(s->flags & EFD_SEMAPHORE);
    const int nonblock = !!(self->flags & 0x800 /*O_NONBLOCK*/) || (s->flags & EFD_NONBLOCK);

    uint64_t old = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    for (;;) {
        while (old == 0) {
            if (nonblock) return -EAGAIN;
            // Block on read_wq until a writer bumps count.
            WAIT_EVENT(&s->read_wq,
                       __atomic_load_n(&s->count, __ATOMIC_ACQUIRE) != 0);
            old = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
        }
        uint64_t taken = sema ? 1 : old;
        uint64_t want  = sema ? (old - 1) : 0;
        if (__atomic_compare_exchange_n(&s->count, &old, want,
                                          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            // Wake a writer who might have been waiting for room.
            if (old == EVENTFD_MAX) wait_queue_wake_all(&s->write_wq);
            __builtin_memcpy(buf, &taken, 8);
            return 8;
        }
        // CAS failed → old reloaded; retry.
    }
}

static int64_t eventfd_write_op(vfs_file_t* self, const void* buf, uint64_t len) {
    if (len < 8) return -EINVAL;
    uint64_t add;
    __builtin_memcpy(&add, buf, 8);
    if (add == (uint64_t)-1) return -EINVAL;  // 0xff..ff is reserved

    eventfd_state_t* s = (eventfd_state_t*)self->ctx;
    const int nonblock = !!(self->flags & 0x800) || (s->flags & EFD_NONBLOCK);

    uint64_t old = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t sum = old + add;
        int overflow = (sum < old) || (sum > EVENTFD_MAX);
        while (overflow) {
            if (nonblock) return -EAGAIN;
            WAIT_EVENT(&s->write_wq,
                       __atomic_load_n(&s->count, __ATOMIC_ACQUIRE) + add <= EVENTFD_MAX);
            old = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
            sum = old + add;
            overflow = (sum < old) || (sum > EVENTFD_MAX);
        }
        if (__atomic_compare_exchange_n(&s->count, &old, sum,
                                          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (old == 0) wait_queue_wake_all(&s->read_wq);
            return 8;
        }
        // CAS failed; retry with refreshed `old`.
    }
}

static int eventfd_poll_op(vfs_file_t* self, int events) {
    eventfd_state_t* s = (eventfd_state_t*)self->ctx;
    uint64_t c = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    int ready = 0;
    if ((events & POLLIN)  && c != 0)             ready |= POLLIN;
    if ((events & POLLOUT) && c != EVENTFD_MAX)   ready |= POLLOUT;
    return ready;
}

static void eventfd_close_op(vfs_file_t* self) {
    if (self->ctx) { kfree(self->ctx); self->ctx = NULL; }
    kfree(self);
}

// ── Constructor exposed to syscall layer ──────────────────────────────
vfs_file_t* eventfd_new(uint32_t init_val, uint32_t flags) {
    eventfd_state_t* s = (eventfd_state_t*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->count = init_val;
    s->flags = flags;
    wait_queue_init(&s->read_wq);
    wait_queue_init(&s->write_wq);

    vfs_file_t* f = vfs_anon_fd(&s->read_wq);   // poll/epoll: read readiness (POLLIN)
    if (!f) { kfree(s); return NULL; }
    f->read  = eventfd_read_op;
    f->write = eventfd_write_op;
    f->poll  = eventfd_poll_op;
    f->close = eventfd_close_op;
    f->ctx   = s;
    // Write readiness (POLLOUT) fires on write_wq when a reader drains a full
    // eventfd.  Expose it as the secondary waitq so sys_poll/sys_select (which
    // register on both f->waitq and f->secondary_waitq) wake a POLLOUT waiter;
    // otherwise a task polling a FULL eventfd for POLLOUT starves forever.
    f->secondary_waitq = &s->write_wq;
    f->flags    = (flags & EFD_NONBLOCK) ? 0x800 : 0;
    return f;
}

// Match predicate used by sys_close to recognize an eventfd.  Comparing
// the close op is cheaper than a type field.
int eventfd_is(vfs_file_t* f) {
    return f && f->close == eventfd_close_op;
}

// ── Boot-time selftest ────────────────────────────────────────────────
// Exercises the vfs ops directly (skipping fd_install so we don't need
// a process context).  Covers: counter accumulation, semaphore mode,
// EAGAIN on empty (nonblock), EINVAL guards, poll readiness, and
// overflow guard on reserved (uint64_t)-1.
#include "kprintf.h"

extern void kprintf_atomic(const char* fmt, ...);

static int efd_write(vfs_file_t* f, uint64_t v) {
    return (int)f->write(f, &v, 8);
}
static int64_t efd_read(vfs_file_t* f) {
    uint64_t v = 0;
    int64_t r = f->read(f, &v, 8);
    return (r == 8) ? (int64_t)v : r;  // negative on error
}

void eventfd_selftest(void) {
    // Case 1: counter mode — two writes accumulate, one read drains.
    vfs_file_t* f = eventfd_new(0, EFD_NONBLOCK);
    if (!f) { kprintf_atomic("[eventfd-selftest] FAIL alloc\n"); return; }
    if (efd_write(f, 3) != 8 || efd_write(f, 5) != 8) {
        kprintf_atomic("[eventfd-selftest] FAIL write\n"); f->close(f); return;
    }
    int64_t got = efd_read(f);
    if (got != 8) {
        kprintf_atomic("[eventfd-selftest] FAIL counter read = %ld\n", got);
        f->close(f); return;
    }
    // After drain, nonblocking read must return -EAGAIN.
    if (efd_read(f) != -EAGAIN) {
        kprintf_atomic("[eventfd-selftest] FAIL expected EAGAIN on empty\n");
        f->close(f); return;
    }
    // Poll: empty counter → no POLLIN, has POLLOUT.
    int p = f->poll(f, POLLIN | POLLOUT);
    if (p != POLLOUT) {
        kprintf_atomic("[eventfd-selftest] FAIL empty poll = %d\n", p);
        f->close(f); return;
    }
    f->close(f);

    // Case 2: semaphore mode — each read decrements by 1.
    f = eventfd_new(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (!f) { kprintf_atomic("[eventfd-selftest] FAIL alloc sema\n"); return; }
    if (efd_write(f, 3) != 8) {
        kprintf_atomic("[eventfd-selftest] FAIL sema write\n"); f->close(f); return;
    }
    for (int i = 0; i < 3; i++) {
        int64_t v = efd_read(f);
        if (v != 1) {
            kprintf_atomic("[eventfd-selftest] FAIL sema read #%d = %ld\n", i, v);
            f->close(f); return;
        }
    }
    if (efd_read(f) != -EAGAIN) {
        kprintf_atomic("[eventfd-selftest] FAIL sema EAGAIN\n");
        f->close(f); return;
    }
    f->close(f);

    // Case 3: reserved value (uint64_t)-1 must be rejected with EINVAL.
    f = eventfd_new(0, EFD_NONBLOCK);
    if (!f) { kprintf_atomic("[eventfd-selftest] FAIL alloc rv\n"); return; }
    if (efd_write(f, (uint64_t)-1) != -EINVAL) {
        kprintf_atomic("[eventfd-selftest] FAIL reserved -1 accept\n");
        f->close(f); return;
    }
    f->close(f);

    // Case 4: poll readiness after a write → POLLIN set.
    f = eventfd_new(0, EFD_NONBLOCK);
    if (!f) { kprintf_atomic("[eventfd-selftest] FAIL alloc p\n"); return; }
    efd_write(f, 1);
    p = f->poll(f, POLLIN | POLLOUT);
    if (!(p & POLLIN) || !(p & POLLOUT)) {
        kprintf_atomic("[eventfd-selftest] FAIL poll after write = %d\n", p);
        f->close(f); return;
    }
    f->close(f);

    // Case 5: POLLOUT readiness must be wired to write_wq.  A full eventfd is
    // not writable; the POLLOUT wakeup (fired when a reader drains it) goes to
    // write_wq, so poll/select must register a POLLOUT waiter there via
    // f->secondary_waitq -- otherwise a task polling a FULL eventfd for POLLOUT
    // starves.  Verify the full-eventfd readiness + the secondary_waitq wiring.
    f = eventfd_new(0, EFD_NONBLOCK);
    if (!f) { kprintf_atomic("[eventfd-selftest] FAIL alloc full\n"); return; }
    eventfd_state_t* s5 = (eventfd_state_t*)f->ctx;
    s5->count = EVENTFD_MAX;   // force full (eventfd_new's init_val is only uint32)
    p = f->poll(f, POLLIN | POLLOUT);
    if ((p & POLLOUT) || !(p & POLLIN)) {
        kprintf_atomic("[eventfd-selftest] FAIL full poll = %d (want POLLIN, no POLLOUT)\n", p);
        f->close(f); return;
    }
    if (f->secondary_waitq != &s5->write_wq) {
        kprintf_atomic("[eventfd-selftest] FAIL POLLOUT not wired to write_wq\n");
        f->close(f); return;
    }
    f->close(f);

    kprintf_atomic("[eventfd-selftest] PASS (counter + semaphore + EAGAIN + EINVAL + poll + POLLOUT-wiring)\n");
}
