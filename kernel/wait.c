#include "wait.h"
#include "sched.h"

// ── wake callbacks ────────────────────────────────────────────────────────
//
// task_wake_func: wake the sleeping task and return WQ_REMOVE so the entry
// is unlinked from the queue.  Used by poll(), select(), blocking reads, etc.
//
// epoll_wake_func: notify the epoll instance that a watched fd is ready, then
// return WQ_KEEP so the entry stays in the file's queue — epoll watches are
// permanent until epoll_ctl(DEL) or fd close.

int task_wake_func(wake_entry_t* we) {
    task_we_t* twe = (task_we_t*)we;
    if (twe->task) {
        sched_wake(twe->task);
        twe->task = 0;   // idempotent — clear so double-wakes are harmless
    }
    return WQ_REMOVE;
}

int epoll_wake_func(wake_entry_t* we) {
    epoll_we_t* ewe = (epoll_we_t*)we;
    *ewe->p_has_ready = 1;
    wait_queue_wake_all(ewe->epoll_wq);  // wake anyone sleeping on the epoll fd
    return WQ_KEEP;                      // stay in the file's waitq forever
}
