#pragma once
// ── Wait queues — Linux-style callback design ─────────────────────────────
//
// Architecture (mirrors Linux exactly, simplified for single-CPU):
//
//   wait_queue_t        — list head, embedded in any waitable object
//                         (vfs_file_t.waitq, tty_t.waitq, epoll_state_t.wq …)
//
//   wake_entry_t        — a node in a wait_queue_t.  Contains a func callback
//                         that is invoked by wait_queue_wake_all/one().
//                         func() returns:
//                           WQ_REMOVE — remove this entry from the queue
//                           WQ_KEEP   — leave this entry in the queue
//                         This one distinction covers every use case:
//                           • direct task sleep (poll/select/read): REMOVE
//                           • epoll watch (persistent): KEEP — entry stays in
//                             the file's waitq forever, until epoll_ctl(DEL)
//
//   task_we_t           — concrete wake_entry for a sleeping task.
//                         Wakes the task, removes itself (WQ_REMOVE).
//                         Heap-alloc exactly one per watched fd in poll/select.
//                         No per-wakeup allocation for epoll.
//
//   epoll_we_t          — concrete wake_entry for an epoll watch (persistent).
//                         On fire: sets has_ready=1, wakes epoll's own wq.
//                         Returns WQ_KEEP — stays in file's queue forever.
//                         Allocated once at epoll_ctl(ADD), freed at DEL/close.
//
// epoll_wait() flow (zero per-wakeup allocation):
//   1. Scan watched fds for ready events — return if any.
//   2. Add ONE task_we_t to epoll_state->wq (task sleeps on epoll's queue).
//   3. Re-check for ready events (close the race window).
//   4. sched_sleep().
//   5. Remove task_we_t from epoll_state->wq.
//   6. Go to 1.
//   When any watched fd fires: epoll_we_t.func sets has_ready, wakes wq.
//   The sleeping task wakes from epoll_state->wq and loops to scan.
//
// poll()/select() flow (one small heap alloc per watched fd, freed on return):
//   Allocate one task_we_t per fd, add to each fd's waitq, sleep, free all.
//   Typical nfds=2-4, so 2-4 allocs total — negligible.

#define WQ_REMOVE 1   // remove entry from queue after wake
#define WQ_KEEP   0   // leave entry in queue (persistent watches)

struct task_t;  // forward decl

// ── Core queue types ──────────────────────────────────────────────────────

typedef struct wake_entry {
    int (*func)(struct wake_entry* we);  // wake callback; returns WQ_REMOVE/WQ_KEEP
    struct wake_entry* next;
    struct wake_entry* prev;
} wake_entry_t;

typedef struct {
    wake_entry_t* head;
} wait_queue_t;

// ── Concrete entry types ──────────────────────────────────────────────────

// task_we_t: for a task sleeping on a queue (poll, select, blocking ops).
// One-shot: func wakes the task and returns WQ_REMOVE.
typedef struct {
    wake_entry_t    we;
    struct task_t*  task;
} task_we_t;

// epoll_we_t: persistent watch entry, lives in a file's waitq.
// func wakes the epoll's own wq and returns WQ_KEEP.
typedef struct {
    wake_entry_t    we;
    wait_queue_t*   epoll_wq;    // &epoll_state->wq
    int*            p_has_ready; // set to 1 so epoll_wait knows to re-scan
} epoll_we_t;

// ── wake callbacks ────────────────────────────────────────────────────────

// Defined out-of-line in wait.c to avoid circular includes (needs sched_wake).
// Declared here so wait.h is self-contained for users.
int task_wake_func(wake_entry_t* we);
int epoll_wake_func(wake_entry_t* we);

// ── Queue primitives ──────────────────────────────────────────────────────

static inline void wait_queue_init(wait_queue_t* wq) { wq->head = 0; }

static inline void wq_add(wait_queue_t* wq, wake_entry_t* we) {
    we->next = we->prev = 0;
    if (!wq->head) { wq->head = we; return; }
    wake_entry_t* tail = wq->head;
    while (tail->next) tail = tail->next;
    tail->next = we;
    we->prev   = tail;
}

// Remove an entry from a queue.  Safe to call if already removed (no-op).
static inline void wq_remove(wait_queue_t* wq, wake_entry_t* we) {
    // Find the entry (short queues — O(n) is fine).
    wake_entry_t* cur = wq->head;
    while (cur && cur != we) cur = cur->next;
    if (!cur) return;

    if (we->prev) we->prev->next = we->next;
    else          wq->head       = we->next;
    if (we->next) we->next->prev = we->prev;
    we->next = we->prev = 0;
}

// Wake all entries on the queue.
// Entries returning WQ_REMOVE are unlinked; WQ_KEEP entries stay.
static inline void wait_queue_wake_all(wait_queue_t* wq) {
    wake_entry_t* we = wq->head;
    while (we) {
        wake_entry_t* next = we->next;
        int remove = we->func(we);
        if (remove) {
            // Unlink this entry.
            if (we->prev) we->prev->next = next;
            else          wq->head       = next;
            if (next) next->prev = we->prev;
            we->next = we->prev = 0;
        }
        we = next;
    }
}

// Wake the first entry only.
static inline void wait_queue_wake_one(wait_queue_t* wq) {
    wake_entry_t* we = wq->head;
    if (!we) return;
    int remove = we->func(we);
    if (remove) {
        wq->head = we->next;
        if (wq->head) wq->head->prev = 0;
        we->next = we->prev = 0;
    }
}

// ── task_we_t helpers ─────────────────────────────────────────────────────

static inline void task_we_init(task_we_t* twe, struct task_t* task) {
    twe->we.func = task_wake_func;
    twe->we.next = twe->we.prev = 0;
    twe->task    = task;
}

// Add a task_we_t to a queue; also records which queue for removal.
// (task_we_t doesn't store the queue — caller must pass wq to wq_remove.)
static inline void task_we_add(wait_queue_t* wq, task_we_t* twe) {
    wq_add(wq, &twe->we);
}

static inline void task_we_remove(wait_queue_t* wq, task_we_t* twe) {
    wq_remove(wq, &twe->we);
}

// ── epoll_we_t helpers ────────────────────────────────────────────────────

static inline void epoll_we_init(epoll_we_t* ewe, wait_queue_t* epoll_wq,
                                  int* p_has_ready) {
    ewe->we.func     = epoll_wake_func;
    ewe->we.next     = ewe->we.prev = 0;
    ewe->epoll_wq    = epoll_wq;
    ewe->p_has_ready = p_has_ready;
}

static inline void epoll_we_add(wait_queue_t* wq, epoll_we_t* ewe) {
    wq_add(wq, &ewe->we);
}

static inline void epoll_we_remove(wait_queue_t* wq, epoll_we_t* ewe) {
    wq_remove(wq, &ewe->we);
}
