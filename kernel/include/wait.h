#pragma once
#include "common.h"
#include "smp.h"

// ── Wait queues — lock-free MPSC design ─────────────────────────────────
//
// A wait_queue_t is a lock-free stack of wake_entry_t nodes.  Multiple
// producers (any CPU) can push concurrently via compare-and-swap on the
// head.  A single consumer (the waker) drains the whole chain with one
// atomic xchg.  Push and drain take zero spinlocks.
//
// Wake semantics are "wake all": the drainer takes the entire list, then
// walks the linked chain calling func() on each entry.  Entries marked
// `dead` (cancelled) are skipped without calling func.
//
// FIFO vs LIFO: push-to-head means drained order is LIFO.  This is fine
// for wake-all semantics (all waiters get woken, ordering doesn't matter)
// and it's the fastest lock-free design.  Wake-one still picks the head.
//
// Cancellation: see the "Removal" section below.  In short, removal is
// serialized via a tiny per-queue spinlock that's only taken on the cold
// cancel path; the hot wake_all path is lock-free.
//
// ── Entry lifetime rules ────────────────────────────────────────────────
//
// A wake_entry_t must remain valid for as long as it may be referenced
// by a drainer.  Two patterns:
//
//   1. Stack-allocated (task_we_t for blocking sleepers):
//      - Caller puts the entry on the stack of the function that calls
//        sched_sleep.
//      - The drainer wakes the task via sched_wake().  The task resumes
//        and eventually returns from sched_sleep, at which point the
//        stack frame (and the entry) can be dropped.
//      - Because sched_wake is called while the drainer is still
//        walking the entry chain, the stack frame is still live.  Safe.
//
//   2. Heap-allocated (epoll_we_t for persistent watches):
//      - Caller kmallocs the entry on epoll_ctl(ADD).
//      - The entry stays in the queue until epoll_ctl(DEL) or fd close,
//        at which point the caller unlinks it via wq_remove() and
//        frees the storage.
//      - wq_remove takes the per-queue spinlock, so there's no race
//        with a concurrent drainer.
//
// ── The spinlock ─────────────────────────────────────────────────────────
//
// Each wait_queue_t has one `spinlock_t remove_lock`.  It's held only in
// wq_remove() to safely unlink a single entry from the middle of the
// list.  It's never held on the push or drain path.  Under normal
// operation (push, wake_all, wake_one) the queue is entirely lock-free.
//
// Justification for this lock (per docs/LOCKS.md rules):
//   1. Hot path is the drain (wake_all), which is lock-free.  Push is
//      also lock-free.  The lock only covers the cold cancel path.
//   2. Lock-free cancel would require either:
//      - a full RCU quiescence wait (too expensive for every wq_remove)
//      - hazard pointers (complex, bounded memory)
//      - heap-allocating every wake_entry and using call_rcu to free
//        (slow, defeats the stack-allocation optimisation)
//      The spinlock on the cold path is the cleanest choice.
//   3. Critical section is a linked-list walk + one pointer swap.
//      Bounded, short, no sleeps.
//   4. wq_remove is never called from IRQ context.  All removers are
//      epoll_ctl(DEL) or polling-loop cleanups in process context.
//      Plain spin_lock is correct (no irqsave needed).

#define WQ_REMOVE 1   // remove entry from queue after wake (one-shot)
#define WQ_KEEP   0   // leave entry in queue (persistent watches)

struct task_t;

// ── Core types ───────────────────────────────────────────────────────────

typedef struct wake_entry {
    int (*func)(struct wake_entry* we);  // wake callback
    struct wake_entry*  next;             // linked-list next in MPSC chain
    volatile uint32_t   dead;             // non-zero = skip on drain
} wake_entry_t;

typedef struct wait_queue_t {
    // Head of the MPSC chain.  Readers (waker) atomically xchg this to
    // drain the entire list.  Writers (sleepers) CAS-push new entries.
    wake_entry_t* volatile head;
    // Guards wq_remove — see comment above.  Never held on push or drain.
    spinlock_t remove_lock;
} wait_queue_t;

// ── Concrete entry types ─────────────────────────────────────────────────

// One-shot task waiter.  Stack-allocated by the caller.
typedef struct {
    wake_entry_t    we;
    struct task_t*  task;
} task_we_t;

// Persistent epoll watch.  Heap-allocated in epoll_ctl(ADD).
typedef struct {
    wake_entry_t    we;
    struct wait_queue_t* epoll_wq;   // &epoll_state->wq
    int*            p_has_ready;     // set to 1 so epoll_wait re-scans
} epoll_we_t;

// ── Wake callbacks (defined in wait.c — need sched_wake) ────────────────
int task_wake_func(wake_entry_t* we);
int epoll_wake_func(wake_entry_t* we);

// ── Queue primitives ─────────────────────────────────────────────────────

ALWAYS_INLINE void wait_queue_init(wait_queue_t* wq) {
    wq->head = NULL;
    spin_lock_init(&wq->remove_lock);
}

// Push an entry onto the queue.  Lock-free MPSC push via CAS.
//
// Invariant: the entry must not already be in a queue.  Callers who
// re-register after a wake must re-init (reset we->dead and we->next).
ALWAYS_INLINE void wq_add(wait_queue_t* wq, wake_entry_t* we) {
    we->dead = 0;
    wake_entry_t* old_head;
    do {
        old_head = atomic_load_relaxed(&wq->head);
        we->next = old_head;
    } while (!__atomic_compare_exchange_n(&wq->head, &old_head, we, 1,
                                             __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

// Remove an entry from the queue.  Serialized via remove_lock so the
// unlink can't race with another remover.  Concurrent drainers are
// handled separately: wq_remove marks the entry dead before walking,
// and if a drainer has already grabbed the chain the entry's `dead`
// flag makes the drainer skip the callback.
//
// Safe to call if the entry has already been drained (no-op in that
// case: walk finds nothing to unlink).
void wq_remove(wait_queue_t* wq, wake_entry_t* we);

// Wake every entry currently in the queue.  Lock-free drain: one xchg
// grabs the whole chain, then we walk it privately.
//
// WQ_KEEP entries (persistent watches) are re-pushed onto the queue
// after wake.  WQ_REMOVE entries are dropped.
void wait_queue_wake_all(wait_queue_t* wq);

// Wake exactly one entry (the head of the chain).  Used where
// "wake the next writer" semantics are needed — typically unused in
// MakaOS but kept for API compatibility.
void wait_queue_wake_one(wait_queue_t* wq);

// ── task_we_t helpers ────────────────────────────────────────────────────

ALWAYS_INLINE void task_we_init(task_we_t* twe, struct task_t* task) {
    twe->we.func = task_wake_func;
    twe->we.next = NULL;
    twe->we.dead = 0;
    twe->task    = task;
}

ALWAYS_INLINE void task_we_add(wait_queue_t* wq, task_we_t* twe) {
    wq_add(wq, &twe->we);
}

ALWAYS_INLINE void task_we_remove(wait_queue_t* wq, task_we_t* twe) {
    wq_remove(wq, &twe->we);
}

// ── epoll_we_t helpers ───────────────────────────────────────────────────

ALWAYS_INLINE void epoll_we_init(epoll_we_t* ewe, wait_queue_t* epoll_wq,
                                  int* p_has_ready) {
    ewe->we.func     = epoll_wake_func;
    ewe->we.next     = NULL;
    ewe->we.dead     = 0;
    ewe->epoll_wq    = epoll_wq;
    ewe->p_has_ready = p_has_ready;
}

ALWAYS_INLINE void epoll_we_add(wait_queue_t* wq, epoll_we_t* ewe) {
    wq_add(wq, &ewe->we);
}

ALWAYS_INLINE void epoll_we_remove(wait_queue_t* wq, epoll_we_t* ewe) {
    wq_remove(wq, &ewe->we);
}
