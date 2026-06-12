// ── signalfd — receive signals via a file descriptor ────────────────
//
// Linux-compatible: signalfd(fd, &mask, flags) returns a fd; reading
// it drains one (or more) pending signals that match `mask` and are
// currently BLOCKED in sigprocmask, returning a signalfd_siginfo per
// signal.  Signals that are unblocked deliver normally (through the
// handler or SIG_DFL); signalfd consumes only the blocked ones — that
// is the exact Linux contract.
//
// Wake mechanism: signal_send_hook iterates the task's signalfd_head
// list and wakes any whose mask covers the incoming signal.  We only
// keep a single-linked list because signalfd subscriptions are rare
// (<4 per process in practice — wayland uses one, systemd one).
// O(n) walk with n < 8 is cheaper than a hashed lookup.

#include "common.h"
#include "kheap.h"
#include "wait.h"
#include "smp.h"
#include "preempt.h"
#include "cpu.h"
#include "sched.h"
#include "process.h"
#include "signal.h"
#include "vfs.h"
#include "syscall.h"
#include "errno.h"
#include "kprintf.h"

extern void kprintf_atomic(const char* fmt, ...);

typedef struct signalfd_state {
    uint32_t      mask;          // sigset bitmap (1<<(sig-1))
    wait_queue_t  wq;
    task_t*       owner;         // owning task (for pending bit lookup)
    struct signalfd_state* next;
} signalfd_state_t;

// Guards every task's signalfd_head list (link, unlink, walk).  All
// three paths are cold (signalfd_new/close, fatal-signal notify), so
// one global IRQ-safe lock is plenty — and it closes the race where a
// close() unlinks a node while signal_send's notify walk is mid-list
// (observed as a nested #PF → panic when a crashing foot thread was
// being killed while another thread tore its signalfd down).
static spinlock_t s_sfd_lock = SPINLOCK_INIT;

// Layout must match userspace <sys/signalfd.h> struct signalfd_siginfo.
// We only populate the fields a typical reader needs; the rest are zero.
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t  __pad[46];
} signalfd_siginfo_t;

_Static_assert(sizeof(signalfd_siginfo_t) == 128,
               "signalfd_siginfo layout must be 128 bytes (Linux ABI)");

// ── Try to claim one pending+masked+blocked signal.  Returns the
//    signal number (1-NSIG) or 0 if nothing available.  Atomically
//    clears the pending bit so two readers don't return the same
//    signal. ────────────────────────────────────────────────────
static int sfd_claim_one(signalfd_state_t* s) {
    if (!s->owner) return 0;
    sigstate_t* ss = &s->owner->sigstate;
    uint32_t eligible;
    for (;;) {
        uint32_t pending = __atomic_load_n(&ss->pending, __ATOMIC_ACQUIRE);
        eligible = pending & ss->blocked & s->mask;
        if (!eligible) return 0;
        int sig = __builtin_ctz(eligible) + 1;
        uint32_t bit = 1u << (sig - 1);
        uint32_t prev = __atomic_fetch_and(&ss->pending, ~bit,
                                             __ATOMIC_ACQ_REL);
        if (prev & bit) return sig;
        // Lost the race — another reader/delivery got this one; retry.
    }
}

// ── vfs ops ─────────────────────────────────────────────────────────
static int64_t signalfd_read_op(vfs_file_t* self, void* buf, uint64_t len) {
    if (len < sizeof(signalfd_siginfo_t)) return -EINVAL;
    signalfd_state_t* s = (signalfd_state_t*)self->ctx;
    const int nonblock = !!(self->flags & 0x800 /*O_NONBLOCK*/);

    uint8_t* out = (uint8_t*)buf;
    uint64_t written = 0;
    uint64_t room = len / sizeof(signalfd_siginfo_t);
    for (uint64_t i = 0; i < room; i++) {
        int sig = sfd_claim_one(s);
        while (!sig && written == 0) {
            if (nonblock) return -EAGAIN;
            WAIT_EVENT(&s->wq,
                (sig = sfd_claim_one(s)) != 0);
            break;
        }
        if (!sig) break;   // drained what we had, room remaining
        signalfd_siginfo_t info = {0};
        info.ssi_signo = (uint32_t)sig;
        info.ssi_pid   = s->owner->pid;
        __builtin_memcpy(out + written * sizeof(info), &info, sizeof(info));
        written++;
    }
    return (int64_t)(written * sizeof(signalfd_siginfo_t));
}

static int64_t signalfd_write_op(vfs_file_t* self, const void* buf,
                                   uint64_t len) {
    (void)self; (void)buf; (void)len;
    return -EINVAL;      // read-only fd
}

static int signalfd_poll_op(vfs_file_t* self, int events) {
    signalfd_state_t* s = (signalfd_state_t*)self->ctx;
    int ready = 0;
    if ((events & POLLIN) && s->owner) {
        sigstate_t* ss = &s->owner->sigstate;
        uint32_t eligible = __atomic_load_n(&ss->pending, __ATOMIC_ACQUIRE)
                          & ss->blocked & s->mask;
        if (eligible) ready |= POLLIN;
    }
    return ready;
}

static void signalfd_close_op(vfs_file_t* self) {
    signalfd_state_t* s = (signalfd_state_t*)self->ctx;
    if (s) {
        // Unlink from owner's subscriber list.
        if (s->owner) {
            uint64_t fl = spin_lock_irqsave(&s_sfd_lock);
            signalfd_state_t** pp = (signalfd_state_t**)&s->owner->signalfd_head;
            while (*pp) {
                if (*pp == s) { *pp = s->next; break; }
                pp = &(*pp)->next;
            }
            spin_unlock_irqrestore(&s_sfd_lock, fl);
        }
        kfree(s);
        self->ctx = NULL;
    }
    kfree(self);
}

// ── Public constructor ───────────────────────────────────────────────
vfs_file_t* signalfd_new(uint32_t mask, uint32_t flags) {
    signalfd_state_t* s = (signalfd_state_t*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    __builtin_memset(s, 0, sizeof(*s));
    s->mask  = mask;
    s->owner = g_current;
    wait_queue_init(&s->wq);

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(*f));
    if (!f) { kfree(s); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));
    f->read  = signalfd_read_op;
    f->write = signalfd_write_op;
    f->poll  = signalfd_poll_op;
    f->close = signalfd_close_op;
    f->ctx   = s;
    f->waitq = &s->wq;
    f->refcount = 1;
    f->flags    = (flags & 0x800) ? 0x800 : 0;   // O_NONBLOCK bit

    // Link into owner's subscriber list (head-insert, O(1)).
    if (g_current) {
        uint64_t fl = spin_lock_irqsave(&s_sfd_lock);
        s->next = (signalfd_state_t*)g_current->signalfd_head;
        g_current->signalfd_head = s;
        spin_unlock_irqrestore(&s_sfd_lock, fl);
    }
    return f;
}

// signalfd_update: change the mask of an existing signalfd without
// re-creating it — Linux allows this by calling signalfd(fd, &mask, 0).
int signalfd_update(vfs_file_t* f, uint32_t mask) {
    if (!f || f->close != signalfd_close_op) return -EINVAL;
    signalfd_state_t* s = (signalfd_state_t*)f->ctx;
    s->mask = mask;
    wait_queue_wake_all(&s->wq);   // re-evaluate in case newly eligible
    return 0;
}

int signalfd_is(vfs_file_t* f) { return f && f->close == signalfd_close_op; }

// ── Hook called from signal_send AFTER pending bit is set ────────────
// Walks the target task's signalfd list (head single-linked) and wakes
// any whose mask covers this signal.  Runs in the sender's context
// (may be cross-CPU) — we never free signalfd_state except from the
// owner's close path, so the list is stable here.
void signalfd_notify(task_t* t, int sig) {
    if (!t) return;
    uint32_t bit = 1u << (sig - 1);
    // preempt_disable across the locked walk: wait_queue_wake_all's
    // rcu_read_unlock is a preempt_enable, and sched_wake sets
    // reschedule_pending — without the guard the unlock-at-depth-0
    // context-switches away WHILE HOLDING the global s_sfd_lock, and
    // every later signal_send on any CPU spins forever with IRQs off.
    // Exact same bug class as timerfd_tick / the mouse ISR; the wake
    // cannot move outside the lock (the lock keeps signalfd_state
    // alive against concurrent close).  Direct depth-- at the end.
    preempt_disable();
    uint64_t fl = spin_lock_irqsave(&s_sfd_lock);
    signalfd_state_t* s = (signalfd_state_t*)t->signalfd_head;
    while (s) {
        if (s->mask & bit) wait_queue_wake_all(&s->wq);
        s = s->next;
    }
    spin_unlock_irqrestore(&s_sfd_lock, fl);
    this_cpu()->preempt_depth--;
}

// ── Boot-time selftest ───────────────────────────────────────────────
// Works on g_current's sigstate.  Sets a signal as blocked, posts it,
// reads through a signalfd, verifies we got the expected siginfo.
void signalfd_selftest(void) {
    if (!g_current) {
        kprintf_atomic("[signalfd-selftest] SKIP (no current task)\n");
        return;
    }
    sigstate_t* ss = &g_current->sigstate;

    // Subscribe to SIGUSR1 (sig 10).  Block it so it lands in pending
    // without being delivered as a handler.
    const int SIG_TEST = 10;   // SIGUSR1
    uint32_t bit = 1u << (SIG_TEST - 1);
    uint32_t saved_blocked = ss->blocked;
    ss->blocked |= bit;

    vfs_file_t* f = signalfd_new(bit, 0x800 /*O_NONBLOCK*/);
    if (!f) {
        kprintf_atomic("[signalfd-selftest] FAIL alloc\n");
        ss->blocked = saved_blocked;
        return;
    }

    // Case 1: nonblock on empty returns EAGAIN.
    signalfd_siginfo_t info;
    int64_t r = f->read(f, &info, sizeof(info));
    if (r != -EAGAIN) {
        kprintf_atomic("[signalfd-selftest] FAIL expected EAGAIN r=%ld\n", r);
        goto cleanup;
    }

    // Case 2: post the signal, read returns it.
    __atomic_or_fetch(&ss->pending, bit, __ATOMIC_RELEASE);
    signalfd_notify(g_current, SIG_TEST);
    r = f->read(f, &info, sizeof(info));
    if (r != (int64_t)sizeof(info)) {
        kprintf_atomic("[signalfd-selftest] FAIL read r=%ld\n", r);
        goto cleanup;
    }
    if (info.ssi_signo != (uint32_t)SIG_TEST) {
        kprintf_atomic("[signalfd-selftest] FAIL signo=%u\n", info.ssi_signo);
        goto cleanup;
    }
    // Case 3: pending bit was cleared by the read.
    if (__atomic_load_n(&ss->pending, __ATOMIC_ACQUIRE) & bit) {
        kprintf_atomic("[signalfd-selftest] FAIL pending not cleared\n");
        goto cleanup;
    }
    // Case 4: next read is EAGAIN again.
    r = f->read(f, &info, sizeof(info));
    if (r != -EAGAIN) {
        kprintf_atomic("[signalfd-selftest] FAIL second read r=%ld\n", r);
        goto cleanup;
    }
    // Case 5: write op rejected.
    uint64_t dummy = 0;
    if (f->write(f, &dummy, 8) != -EINVAL) {
        kprintf_atomic("[signalfd-selftest] FAIL write allowed\n");
        goto cleanup;
    }

    kprintf_atomic("[signalfd-selftest] PASS (EAGAIN + signo + drain + no-write)\n");
cleanup:
    f->close(f);
    ss->blocked = saved_blocked;
    // Clear any residual pending for the test signal.
    __atomic_and_fetch(&ss->pending, ~bit, __ATOMIC_RELEASE);
}
