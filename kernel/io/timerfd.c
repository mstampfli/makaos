// ── timerfd — Linux-compatible timer-driven fd ────────────────────────
//
// Per-CPU sorted timer list.  Each timerfd has a home CPU, chosen at
// first settime from the calling task's current CPU.  That CPU's tick
// handler drains expired timers from its own local list — no cross-CPU
// locking on the hot path.
//
// Fast-path (tick): one atomic load of `head_expiry`.  If still in the
// future, return O(1).  Only when head expired do we take the local
// spinlock and drain.  Insertion is settime — cold path, O(n) in the
// number of active timers on that CPU (typically <10).
//
// Supports CLOCK_MONOTONIC and CLOCK_REALTIME (internally both map to
// tsc_read_ns since we don't have wall-clock separation yet).  Both
// relative and ABSTIME modes.  Periodic timers reinsert automatically.
// Missed ticks accumulate into the expirations counter (Linux behaviour).

#include "common.h"
#include "kheap.h"
#include "wait.h"
#include "smp.h"
#include "cpu.h"
#include "sched.h"
#include "process.h"
#include "vfs.h"
#include "syscall.h"
#include "errno.h"
#include "kprintf.h"

extern uint64_t tsc_read_ns(void);
extern void     kprintf_atomic(const char* fmt, ...);

// ── Per-timer state ───────────────────────────────────────────────────
typedef struct timerfd_state {
    int               clockid;
    uint32_t          flags;
    wait_queue_t      wq;

    // Home CPU — the per-CPU list this timer lives on.  Set on first
    // settime; stable for the life of the timer.  ~0u means "no home
    // yet (timer has never been armed)".
    uint32_t          home_cpu;

    // All of these are protected by g_tfd_pcpu[home_cpu].lock except
    // `expirations` which is atomic (read lock-free by read()/poll()).
    uint64_t          next_expiry_ns;   // absolute, in tsc_read_ns timeline
    uint64_t          interval_ns;      // 0 = one-shot
    struct timerfd_state* next;         // sorted singly-linked list
    int               in_list;          // 1 = present in home_cpu's list

    uint64_t          expirations;      // atomic: read() drains
} timerfd_state_t;

// ── Per-CPU list head ─────────────────────────────────────────────────
typedef struct timerfd_percpu {
    spinlock_t        lock;
    timerfd_state_t*  head;           // sorted ascending by next_expiry_ns
    uint64_t          head_expiry;    // atomic cache; UINT64_MAX when empty
} timerfd_percpu_t;

static timerfd_percpu_t g_tfd_pcpu[MAX_CPUS];

static void tfd_pc_init_once(void) {
    static int s_init_done = 0;
    if (__atomic_load_n(&s_init_done, __ATOMIC_ACQUIRE)) return;
    // Racy lazy init — first caller wins; losers see s_init_done==1 and
    // skip.  We use a CAS to serialize the init block.
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_init_done, &expect, 2,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < MAX_CPUS; i++) {
            spin_lock_init(&g_tfd_pcpu[i].lock);
            g_tfd_pcpu[i].head = NULL;
            g_tfd_pcpu[i].head_expiry = ~(uint64_t)0;
        }
        __atomic_store_n(&s_init_done, 1, __ATOMIC_RELEASE);
    } else {
        // Lost the race; wait for winner to flip back to 1.
        while (__atomic_load_n(&s_init_done, __ATOMIC_ACQUIRE) != 1)
            __builtin_ia32_pause();
    }
}

// ── Sorted-list insert (caller holds pc->lock) ───────────────────────
static void tfd_list_insert(timerfd_percpu_t* pc, timerfd_state_t* t) {
    timerfd_state_t** pp = &pc->head;
    while (*pp && (*pp)->next_expiry_ns <= t->next_expiry_ns)
        pp = &(*pp)->next;
    t->next  = *pp;
    *pp      = t;
    t->in_list = 1;
    __atomic_store_n(&pc->head_expiry, pc->head->next_expiry_ns,
                     __ATOMIC_RELEASE);
}

// ── Sorted-list remove (caller holds pc->lock) ───────────────────────
static void tfd_list_remove(timerfd_percpu_t* pc, timerfd_state_t* t) {
    if (!t->in_list) return;
    timerfd_state_t** pp = &pc->head;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp == t) {
        *pp = t->next;
        t->next = NULL;
        t->in_list = 0;
    }
    __atomic_store_n(&pc->head_expiry,
                     pc->head ? pc->head->next_expiry_ns : ~(uint64_t)0,
                     __ATOMIC_RELEASE);
}

// ── Scheduler-tick hook (called on every CPU's tick) ─────────────────
// Fast-path: single atomic load.  Slow-path: drain expired timers on
// THIS CPU's list.  Called from sched_tick — may run in IRQ context,
// so we use irqsave locking.
void timerfd_tick(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) return;
    timerfd_percpu_t* pc = &g_tfd_pcpu[cpu];

    uint64_t now = tsc_read_ns();
    if (__atomic_load_n(&pc->head_expiry, __ATOMIC_ACQUIRE) > now) return;

    uint64_t flags = spin_lock_irqsave(&pc->lock);
    while (pc->head && pc->head->next_expiry_ns <= now) {
        timerfd_state_t* t = pc->head;
        pc->head = t->next;
        t->next = NULL;
        t->in_list = 0;

        // Compute how many intervals have elapsed (Linux behaviour:
        // accumulate missed ticks into expirations counter).
        uint64_t missed = 1;
        if (t->interval_ns) {
            uint64_t past = now - t->next_expiry_ns;
            missed += past / t->interval_ns;
            t->next_expiry_ns += missed * t->interval_ns;
        }
        __atomic_fetch_add(&t->expirations, missed, __ATOMIC_RELEASE);

        if (t->interval_ns) {
            tfd_list_insert(pc, t);
        }
        wait_queue_wake_all(&t->wq);
    }
    // Refresh head_expiry sentinel in case list emptied without reinsert.
    __atomic_store_n(&pc->head_expiry,
                     pc->head ? pc->head->next_expiry_ns : ~(uint64_t)0,
                     __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&pc->lock, flags);
}

// ── vfs_file_t ops ────────────────────────────────────────────────────
static int64_t timerfd_read_op(vfs_file_t* self, void* buf, uint64_t len) {
    if (len < 8) return -EINVAL;
    timerfd_state_t* t = (timerfd_state_t*)self->ctx;
    const int nonblock = !!(self->flags & 0x800 /*O_NONBLOCK*/)
                      || (t->flags & TFD_NONBLOCK);

    for (;;) {
        uint64_t count = __atomic_exchange_n(&t->expirations, 0,
                                             __ATOMIC_ACQ_REL);
        if (count) {
            __builtin_memcpy(buf, &count, 8);
            return 8;
        }
        if (nonblock) return -EAGAIN;
        WAIT_EVENT(&t->wq,
                   __atomic_load_n(&t->expirations, __ATOMIC_ACQUIRE) != 0);
    }
}

static int timerfd_poll_op(vfs_file_t* self, int events) {
    timerfd_state_t* t = (timerfd_state_t*)self->ctx;
    int ready = 0;
    if ((events & POLLIN) &&
        __atomic_load_n(&t->expirations, __ATOMIC_ACQUIRE) != 0)
        ready |= POLLIN;
    // Never POLLOUT — can't write to a timerfd.
    return ready;
}

static void timerfd_close_op(vfs_file_t* self) {
    timerfd_state_t* t = (timerfd_state_t*)self->ctx;
    if (t) {
        if (t->home_cpu < MAX_CPUS) {
            timerfd_percpu_t* pc = &g_tfd_pcpu[t->home_cpu];
            uint64_t flags = spin_lock_irqsave(&pc->lock);
            tfd_list_remove(pc, t);
            spin_unlock_irqrestore(&pc->lock, flags);
        }
        kfree(t);
        self->ctx = NULL;
    }
    kfree(self);
}

// timerfd cannot be written to.
static int64_t timerfd_write_op(vfs_file_t* self, const void* buf,
                                 uint64_t len) {
    (void)self; (void)buf; (void)len;
    return -EINVAL;
}

// ── Public constructor ────────────────────────────────────────────────
vfs_file_t* timerfd_new(int clockid, uint32_t flags) {
    tfd_pc_init_once();
    if (clockid != K_CLOCK_MONOTONIC && clockid != K_CLOCK_REALTIME)
        return NULL;

    timerfd_state_t* t = (timerfd_state_t*)kmalloc(sizeof(*t));
    if (!t) return NULL;
    __builtin_memset(t, 0, sizeof(*t));
    t->clockid  = clockid;
    t->flags    = flags;
    t->home_cpu = ~(uint32_t)0;   // no home until first settime
    wait_queue_init(&t->wq);

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(*f));
    if (!f) { kfree(t); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));
    f->read  = timerfd_read_op;
    f->write = timerfd_write_op;
    f->poll  = timerfd_poll_op;
    f->close = timerfd_close_op;
    f->ctx   = t;
    f->waitq = &t->wq;
    f->refcount = 1;
    f->flags    = (flags & TFD_NONBLOCK) ? 0x800 : 0;
    return f;
}

int timerfd_is(vfs_file_t* f) {
    return f && f->close == timerfd_close_op;
}

// ── settime / gettime ─────────────────────────────────────────────────
// settime: installs new_value on the calling CPU's list.  If a previous
// arming used a different CPU, we first remove from the old home.
int timerfd_settime(vfs_file_t* f, int flags,
                    const k_itimerspec_t* new_val,
                    k_itimerspec_t* old_val) {
    if (!timerfd_is(f) || !new_val) return -EINVAL;
    timerfd_state_t* t = (timerfd_state_t*)f->ctx;

    uint64_t new_interval = (uint64_t)new_val->it_interval.tv_sec * 1000000000ULL
                          + (uint64_t)new_val->it_interval.tv_nsec;
    uint64_t new_value    = (uint64_t)new_val->it_value.tv_sec * 1000000000ULL
                          + (uint64_t)new_val->it_value.tv_nsec;

    uint64_t now = tsc_read_ns();
    uint64_t new_expiry = 0;
    if (new_value) {
        if (flags & TFD_TIMER_ABSTIME) new_expiry = new_value;
        else                           new_expiry = now + new_value;
    }

    // Detach from old home (if any).
    if (t->home_cpu < MAX_CPUS) {
        timerfd_percpu_t* old_pc = &g_tfd_pcpu[t->home_cpu];
        uint64_t old_flags = spin_lock_irqsave(&old_pc->lock);
        // Capture old values for old_val before clearing.
        if (old_val) {
            uint64_t remain = (t->next_expiry_ns > now)
                             ? (t->next_expiry_ns - now) : 0;
            old_val->it_value.tv_sec  = (int64_t)(remain / 1000000000ULL);
            old_val->it_value.tv_nsec = (int64_t)(remain % 1000000000ULL);
            old_val->it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000ULL);
            old_val->it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000ULL);
        }
        tfd_list_remove(old_pc, t);
        t->next_expiry_ns = 0;
        t->interval_ns    = 0;
        spin_unlock_irqrestore(&old_pc->lock, old_flags);
    } else if (old_val) {
        old_val->it_value.tv_sec = old_val->it_value.tv_nsec = 0;
        old_val->it_interval.tv_sec = old_val->it_interval.tv_nsec = 0;
    }

    if (!new_expiry) return 0;   // disarm-only

    // Attach to CURRENT CPU's list.  The calling thread's home is the
    // most-likely CPU for its subsequent read(), keeping the wake local.
    tfd_pc_init_once();
    uint32_t cpu = cpu_id();
    timerfd_percpu_t* pc = &g_tfd_pcpu[cpu];
    uint64_t lflags = spin_lock_irqsave(&pc->lock);
    t->home_cpu       = cpu;
    t->next_expiry_ns = new_expiry;
    t->interval_ns    = new_interval;
    tfd_list_insert(pc, t);
    spin_unlock_irqrestore(&pc->lock, lflags);
    return 0;
}

int timerfd_gettime(vfs_file_t* f, k_itimerspec_t* out) {
    if (!timerfd_is(f) || !out) return -EINVAL;
    timerfd_state_t* t = (timerfd_state_t*)f->ctx;
    uint64_t now = tsc_read_ns();

    if (t->home_cpu < MAX_CPUS) {
        timerfd_percpu_t* pc = &g_tfd_pcpu[t->home_cpu];
        uint64_t lflags = spin_lock_irqsave(&pc->lock);
        uint64_t remain = (t->next_expiry_ns > now)
                         ? (t->next_expiry_ns - now) : 0;
        out->it_value.tv_sec  = (int64_t)(remain / 1000000000ULL);
        out->it_value.tv_nsec = (int64_t)(remain % 1000000000ULL);
        out->it_interval.tv_sec  = (int64_t)(t->interval_ns / 1000000000ULL);
        out->it_interval.tv_nsec = (int64_t)(t->interval_ns % 1000000000ULL);
        spin_unlock_irqrestore(&pc->lock, lflags);
    } else {
        out->it_value.tv_sec = out->it_value.tv_nsec = 0;
        out->it_interval.tv_sec = out->it_interval.tv_nsec = 0;
    }
    return 0;
}

// ── Boot-time selftest ────────────────────────────────────────────────
// Sleeps via busy-wait on tsc_read_ns since we're in init_kthread where
// nanosleep is available but we don't want to tangle with sched_sleep
// in a test harness.  The timerfd_tick runs on our CPU's tick, so a
// 50ms wait is plenty for expirations to show up.
static void tfd_wait_ns(uint64_t ns) {
    uint64_t deadline = tsc_read_ns() + ns;
    while (tsc_read_ns() < deadline) __builtin_ia32_pause();
}

void timerfd_selftest(void) {
    // Case 1: one-shot relative timer fires once.
    vfs_file_t* f = timerfd_new(K_CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (!f) { kprintf_atomic("[timerfd-selftest] FAIL alloc\n"); return; }
    k_itimerspec_t spec = {0};
    spec.it_value.tv_nsec = 10 * 1000 * 1000;   // 10 ms
    if (timerfd_settime(f, 0, &spec, NULL) != 0) {
        kprintf_atomic("[timerfd-selftest] FAIL settime oneshot\n");
        f->close(f); return;
    }
    tfd_wait_ns(50 * 1000 * 1000);   // 50 ms
    uint64_t v = 0;
    int64_t r = f->read(f, &v, 8);
    if (r != 8 || v != 1) {
        kprintf_atomic("[timerfd-selftest] FAIL oneshot r=%ld v=%lu\n", r, v);
        f->close(f); return;
    }
    // Next read must be EAGAIN.
    r = f->read(f, &v, 8);
    if (r != -EAGAIN) {
        kprintf_atomic("[timerfd-selftest] FAIL oneshot EAGAIN = %ld\n", r);
        f->close(f); return;
    }
    f->close(f);

    // Case 2: periodic timer accumulates multiple expirations.
    f = timerfd_new(K_CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (!f) { kprintf_atomic("[timerfd-selftest] FAIL alloc p\n"); return; }
    k_itimerspec_t per = {0};
    per.it_value.tv_nsec    = 5 * 1000 * 1000;   // first fire in 5 ms
    per.it_interval.tv_nsec = 5 * 1000 * 1000;   // then every 5 ms
    if (timerfd_settime(f, 0, &per, NULL) != 0) {
        kprintf_atomic("[timerfd-selftest] FAIL settime periodic\n");
        f->close(f); return;
    }
    tfd_wait_ns(50 * 1000 * 1000);   // 50 ms → expect ~10 expirations
    v = 0;
    r = f->read(f, &v, 8);
    if (r != 8 || v < 5) {
        kprintf_atomic("[timerfd-selftest] FAIL periodic r=%ld v=%lu (want >=5)\n", r, v);
        f->close(f); return;
    }
    f->close(f);

    // Case 3: gettime on disarmed timer returns zero.
    f = timerfd_new(K_CLOCK_MONOTONIC, 0);
    if (!f) { kprintf_atomic("[timerfd-selftest] FAIL alloc g\n"); return; }
    k_itimerspec_t g = { .it_interval={1,1}, .it_value={1,1} };
    if (timerfd_gettime(f, &g) != 0
        || g.it_value.tv_sec != 0 || g.it_value.tv_nsec != 0) {
        kprintf_atomic("[timerfd-selftest] FAIL gettime disarmed\n");
        f->close(f); return;
    }
    f->close(f);

    // Case 4: write op rejected.
    f = timerfd_new(K_CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (!f) { kprintf_atomic("[timerfd-selftest] FAIL alloc w\n"); return; }
    uint64_t wv = 1;
    if (f->write(f, &wv, 8) != -EINVAL) {
        kprintf_atomic("[timerfd-selftest] FAIL write should reject\n");
        f->close(f); return;
    }
    f->close(f);

    kprintf_atomic("[timerfd-selftest] PASS (oneshot + periodic + gettime + no-write)\n");
}
