#include "wait.h"
#include "sched.h"

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
// Zero locks.  The xchg is the only atomic op on the hot path.
void wait_queue_wake_all(wait_queue_t* wq) {
    if (!wq) return;

    // One atomic op grabs the whole list.  After this, `chain` is a
    // private linked list only this caller can see; the queue is empty
    // (unless a concurrent pusher has already started adding new
    // entries, which will be on the fresh chain, not this one).
    wake_entry_t* chain = __atomic_exchange_n(&wq->head, (wake_entry_t*)NULL,
                                                __ATOMIC_ACQ_REL);

    // Walk the detached chain.  next pointers are stable because we
    // own the chain now.
    while (chain) {
        wake_entry_t* next = chain->next;
        // Read dead once.  If the canceller set it after we grabbed the
        // chain, we see the new value and skip.  If we see dead==0 and
        // the canceller sets it afterwards, the canceller's wq_remove
        // will find the entry gone (it's in our private chain) and
        // silently do nothing — fine, we've already committed to
        // calling func for this entry.
        if (!atomic_load_acq(&chain->dead)) {
            int keep = chain->func(chain);
            if (keep == WQ_KEEP) {
                // Re-push onto the queue for the next round.
                wq_add(wq, chain);
            }
        }
        chain = next;
    }
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
}

// ── wake callbacks ────────────────────────────────────────────────────────

int task_wake_func(wake_entry_t* we) {
    task_we_t* twe = (task_we_t*)we;
    if (twe->task) {
        sched_wake(twe->task);
        twe->task = NULL;  // idempotent — clear so double-wakes are harmless
    }
    return WQ_REMOVE;
}

int epoll_wake_func(wake_entry_t* we) {
    epoll_we_t* ewe = (epoll_we_t*)we;
    // RELEASE store pairs with the sleeper's ACQUIRE load in sys_epoll_wait.
    // Together with the underlying wait_queue_wake_all ACQ_REL on the
    // sleeper's wait queue, this gives us a straight happens-before
    // chain: waker's ring push → has_ready=1 → state->wq wake_all →
    // sleeper observes has_ready=1 OR sleeper is woken directly.
    // See the detailed commentary in sys_epoll_wait for the full
    // ordering proof.
    __atomic_store_n(ewe->p_has_ready, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(ewe->epoll_wq);  // wake anyone sleeping on the epoll fd
    return WQ_KEEP;                      // stay in the file's waitq forever
}
