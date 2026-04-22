#include "wait.h"
#include "sched.h"
#include "rcu.h"

// ── Wait queue — lock-free drain, lock-free push, locked remove ──────────
//
// See kernel/include/wait.h for the full design.  This file has three
// out-of-line functions:
//   - wq_remove          (cold, takes the per-queue remove_lock)
//   - wait_queue_wake_all (hot, entirely lock-free)
//   - wait_queue_wake_one (hot, entirely lock-free)
//
// Push lives inline in the header as wq_add — just a CAS.

// ── wq_remove ────────────────────────────────────────────────────────────
//
// Mark the entry dead (so any drainer that already grabbed the chain
// will skip the callback), then unlink it from the head-chained list
// under the queue's remove_lock.
//
// After this function returns, the entry is either:
//   - out of the queue entirely (we unlinked it), or
//   - in a drainer's private chain with dead=1 (drainer will skip it).
// In both cases, the callback will not be invoked.  The caller can
// safely free the entry's storage if it's heap-allocated.
void wq_remove(wait_queue_t* wq, wake_entry_t* we) {
    if (!wq || !we) return;

    // Mark dead first.  If a drainer xchg's the chain between this
    // store and our pointer unlink below, it will see dead=1 and
    // skip the callback — we're safe either way.
    atomic_store_rel(&we->dead, 1u);

    spin_lock(&wq->remove_lock);
    // Walk the chain looking for our entry.  Chains are short — typical
    // wait queue has 1–4 entries — so an O(n) walk is fine.
    wake_entry_t* volatile* link = &wq->head;
    wake_entry_t* cur = atomic_load_relaxed(link);
    while (cur && cur != we) {
        link = &cur->next;
        cur  = cur->next;
    }
    if (cur == we) {
        // Unlink.  We hold the remove_lock so no other remover can
        // touch these pointers concurrently.  A concurrent drainer
        // could xchg the head, but that just means we're looking at
        // a detached chain — fine, the unlink still works on the
        // detached portion (only if cur happens to be still in there).
        // If the drainer already grabbed cur, our link==&wq->head
        // would have failed to find cur and we'd exit via cur==NULL.
        *link = cur->next;
    }
    spin_unlock(&wq->remove_lock);
}

// ── wait_queue_wake_all ──────────────────────────────────────────────────
//
// Atomically detach the entire chain from the queue, then walk it.
// Each live entry (dead==0) has its func invoked.  Entries returning
// WQ_KEEP are re-pushed onto the queue.
//
// Lifetime / RCU discipline:
//
//   A wait_queue_t entry may be owned by a heap-allocated struct (e.g.
//   epoll_watch_t) that can be freed concurrently with a drain.  The
//   classic use-after-free:
//
//     CPU A: wait_queue_wake_all(wq)
//              xchg → private `chain` containing entry X
//              walk...                                  (reads X->func)
//     CPU B: close(fd) → wq_remove(wq, X) → kfree(X)
//
//   Between CPU A's xchg and the read of X->func, CPU B runs the
//   close path and kfree's the memory backing X.  CPU A then reads
//   garbage (or, with PMM_DEBUG_ALWAYS_ZERO, exact zeros) from X->func
//   and calls it — kernel #PF on instruction fetch at 0.
//
//   Fix: the drain walk is an RCU reader section.  Every heap-backed
//   wait-queue entry must be freed via call_rcu, not plain kfree, so
//   that the actual free is deferred until after any concurrent
//   drainer has completed.  Stack-allocated entries (task_we_t) don't
//   need this — the owning thread can't return from its sleep until
//   the drain has already called sched_wake on it, and the drain
//   finishes its func call before letting go of the entry.
//
//   The hot path is still nearly lock-free: rcu_read_lock is a single
//   pair of inc/dec on a per-CPU counter (no atomic ops on this CPU),
//   and call_rcu is a single atomic push onto a per-CPU deferred list.
//   We pay grace-period latency (~a few ticks) on the cold close path,
//   which is fine.
void wait_queue_wake_all(wait_queue_t* wq) {
    if (!wq) return;

    // One atomic op grabs the whole list.  After this, `chain` is a
    // private linked list only this caller can see; the queue is empty
    // (unless a concurrent pusher has already started adding new
    // entries, which will be on the fresh chain, not this one).
    wake_entry_t* chain = __atomic_exchange_n(&wq->head, (wake_entry_t*)NULL,
                                                __ATOMIC_ACQ_REL);
    if (!chain) return;

    rcu_read_lock();

    // Walk the detached chain.  next pointers are stable because we
    // own the chain now.
    while (chain) {
        // Canonical-pointer sanity: next must be NULL or a kernel
        // higher-half address.  Anything else means the entry's
        // memory was clobbered after enqueue (classic UAF — entry
        // freed by its owner but still linked on our list).  Catch
        // it here with context, rather than letting the next
        // iteration GP on `mov 0x8(%r15),%r15` with a non-canonical
        // pointer in r15.
        if (chain->next != NULL &&
            ((uintptr_t)chain->next >> 47) != 0x1FFFF) {
            extern void kprintf(const char*, ...);
            kprintf("\n=== WAKE UAF next === wq=%p chain=%p func=%p "
                    "dead=%x next=%p\n",
                    wq, chain, chain->func,
                    chain->dead, chain->next);
            chain->next = NULL;  // truncate the chain, don't GP
        }
        wake_entry_t* next = chain->next;
        // Read dead once.  If the canceller set it after we grabbed the
        // chain, we see the new value and skip.  If we see dead==0 and
        // the canceller sets it afterwards, the canceller's wq_remove
        // will find the entry gone (it's in our private chain) and
        // silently do nothing — fine, we've already committed to
        // calling func for this entry.
        if (!atomic_load_acq(&chain->dead)) {
            // Sanity check: corrupt func pointers mean the wake_entry
            // memory has been clobbered.  Catch the bug HERE — with
            // full context — rather than the cryptic downstream #UD.
            extern int task_wake_func(wake_entry_t* we);
            extern int epoll_wake_func(wake_entry_t* we);
            if (chain->func != task_wake_func &&
                chain->func != epoll_wake_func) {
                extern void kprintf(const char*, ...);
                kprintf("\n=== WAKE UAF === wq=%p chain=%p func=%p "
                        "dead=%x next=%p\n",
                        wq, chain, chain->func,
                        chain->dead, chain->next);
                for (;;) __asm__ volatile("cli; hlt");
            }
            int keep = chain->func(chain);
            if (keep == WQ_KEEP) {
                // Re-push onto the queue for the next round.
                wq_add(wq, chain);
            }
        }
        chain = next;
    }

    rcu_read_unlock();
}

// ── wait_queue_wake_one ──────────────────────────────────────────────────
//
// Wake the single most-recently-added entry (LIFO — that's the head).
// Rarely used; kept for API compatibility.
//
// Implementation: grab the chain, fire func on the first live entry,
// re-push the rest.  Simple, correct, not the hot path.
void wait_queue_wake_one(wait_queue_t* wq) {
    if (!wq) return;

    wake_entry_t* chain = __atomic_exchange_n(&wq->head, (wake_entry_t*)NULL,
                                                __ATOMIC_ACQ_REL);
    if (!chain) return;

    rcu_read_lock();

    // Fire func on the first live entry.
    wake_entry_t* fired = NULL;
    while (chain && atomic_load_acq(&chain->dead)) {
        wake_entry_t* next = chain->next;
        chain = next;
    }
    if (chain) {
        fired = chain;
        chain = chain->next;  // advance past the fired entry
    }

    // Re-push everything else (whether live or dead — we leave cancels
    // to be reaped on the next wake_all).
    while (chain) {
        wake_entry_t* next = chain->next;
        wq_add(wq, chain);
        chain = next;
    }

    if (fired) {
        int keep = fired->func(fired);
        if (keep == WQ_KEEP) wq_add(wq, fired);
    }

    rcu_read_unlock();
}

// ── wake callbacks ────────────────────────────────────────────────────────

int task_wake_func(wake_entry_t* we) {
    task_we_t* twe = (task_we_t*)we;
    // Do NOT clear twe->task.  A spurious double-drain of the same we is
    // harmless — sched_wake is idempotent (the 2nd call finds state !=
    // SLEEPING and sets wake_pending, which the task consumes on its
    // next sched_sleep).  Clearing task caused a lost wakeup in this
    // sequence:
    //   1. push we (task_we_init sets task=login, task_we_add pushes)
    //   2. wake_all drains → this func runs → sched_wake(login), clear
    //   3. same stack slot is pushed again (next iteration of slot_alloc
    //      / tty_vfs_read / etc), task_we_init resets task=login
    //   4. another wake_all drains → reads task... and saw NULL somehow
    //      (between steps 3 and 4, something made task read back as NULL
    //      — either a memory ordering artifact visible to a remote CPU,
    //      or the same we being processed twice within a single wake_all
    //      due to a self-loop in chain->next).  Skipping sched_wake here
    //      meant the real wake never fired; login stayed TASK_SLEEPING
    //      on sleep_head forever with wake_pending=0.
    // Leaving task set eliminates that failure mode: every drain that
    // reaches here calls sched_wake on the actual task.
    if (twe->task)
        sched_wake(twe->task);
    return WQ_REMOVE;
}

int epoll_wake_func(wake_entry_t* we) {
    epoll_we_t* ewe = (epoll_we_t*)we;
    __atomic_store_n(ewe->p_has_ready, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(ewe->epoll_wq);
    return WQ_KEEP;
}
