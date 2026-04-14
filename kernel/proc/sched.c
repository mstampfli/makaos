#include "sched.h"
#include "process.h"
#include "signal.h"
#include "timer.h"
#include "tsc.h"
#include "tss.h"
#include "common.h"
#include "kheap.h"
#include "cpu.h"
#include "rcu.h"


// ── MLFQ parameters ───────────────────────────────────────────────────────
// 4 priority levels.  Level 0 is highest (interactive).
// Quantum doubles at each level.  Boost every BOOST_INTERVAL ticks moves
// all tasks back to level 0, preventing starvation of CPU-bound tasks.

#define MLFQ_LEVELS      4
#define BOOST_INTERVAL   1000  // ticks ≈ 1 s at 1000 Hz

static const uint8_t s_quanta[MLFQ_LEVELS] = {8, 16, 32, 64};

// ── Globals ───────────────────────────────────────────────────────────────

// g_current is now a per-CPU accessor macro — see sched.h.  Storage
// lives in cpu_t.current and is written by do_switch.  No global.
task_t* g_init_task = NULL;  // first user process; orphans reparent here

// ── PID -> task hash table (RCU-protected) ──────────────────────────────
//
// Readers (pid_ht_find) take zero locks: rcu_read_lock + rcu_dereference
// of the state pointer, then a plain walk of the slot array.  Writers
// (insert/remove/grow) serialize on s_pid_ht_lock.  Grow publishes the
// new state via rcu_assign_pointer and frees the old state after
// synchronize_rcu so any in-flight reader finishes first.
//
// The state struct bundles {slots, cap, count} so a reader that snapped
// a pointer is guaranteed a consistent (array, capacity) pair — the
// writer never mutates an already-published state, only builds a fresh
// one during grow.
//
// Count updates are done by the writer on the *current* state, protected
// by the spinlock.  Readers don't care about count.
//
// Tombstones (PID_HT_TOMB) mark deleted slots so probe chains survive.
// In-flight readers that see a tombstone just skip it; they never touch
// freed memory because the task_t is only freed after a separate
// synchronize_rcu on exit (handled by sched_add_zombie reclaim, which
// will be RCU-izzed in Phase 6).

#define PID_HT_INIT_CAP   64u
#define PID_HT_TOMB       ((task_t*)1uL)

typedef struct pid_ht_state {
    task_t** slots;
    uint32_t cap;
    uint32_t count;
} pid_ht_state_t;

static pid_ht_state_t* s_pid_ht = NULL;        // published via rcu_assign_pointer
static spinlock_t      s_pid_ht_lock = SPINLOCK_INIT;

static uint32_t pid_hash(uint32_t pid, uint32_t cap) {
    return (pid * 2654435761u) & (cap - 1u);
}

// Allocate a fresh state with the given capacity.  Returns NULL on OOM.
static pid_ht_state_t* pid_ht_alloc_state(uint32_t cap) {
    pid_ht_state_t* st = (pid_ht_state_t*)kmalloc(sizeof(pid_ht_state_t));
    if (!st) return NULL;
    st->slots = (task_t**)kmalloc((uint64_t)cap * sizeof(task_t*));
    if (!st->slots) { kfree(st); return NULL; }
    __builtin_memset(st->slots, 0, (uint64_t)cap * sizeof(task_t*));
    st->cap   = cap;
    st->count = 0;
    return st;
}

// Insert t into slots[] using linear probing.  Caller ensures capacity.
static void pid_ht_raw_insert(task_t** slots, uint32_t cap, task_t* t) {
    uint32_t i = pid_hash(t->pid, cap);
    for (;;) {
        task_t* s = slots[i];
        if (!s || s == PID_HT_TOMB) { slots[i] = t; return; }
        i = (i + 1u) & (cap - 1u);
    }
}

// Lazy initialization — called under the writer lock.
static pid_ht_state_t* pid_ht_ensure_init_locked(void) {
    pid_ht_state_t* st = s_pid_ht;
    if (st) return st;
    st = pid_ht_alloc_state(PID_HT_INIT_CAP);
    if (!st) return NULL;
    rcu_assign_pointer(s_pid_ht, st);
    return st;
}

// Grow the hash table.  Publishes the new state via rcu_assign_pointer,
// then — AFTER releasing the writer lock — calls synchronize_rcu and
// frees the old state.  We must not hold a spinlock across
// synchronize_rcu because that would yield with the lock held.
//
// Returns the new state on success, or NULL on OOM.  Caller holds
// s_pid_ht_lock on entry, still holds it on return.  The old state is
// stashed into *out_old_for_reclaim so the caller can reclaim it after
// releasing the lock.
static pid_ht_state_t* pid_ht_grow_locked(pid_ht_state_t* old,
                                           pid_ht_state_t** out_old_for_reclaim) {
    uint32_t new_cap = old->cap * 2u;
    pid_ht_state_t* ns = pid_ht_alloc_state(new_cap);
    if (!ns) { *out_old_for_reclaim = NULL; return NULL; }
    // Copy all live entries (skip tombstones).
    for (uint32_t i = 0; i < old->cap; i++) {
        task_t* s = old->slots[i];
        if (s && s != PID_HT_TOMB) pid_ht_raw_insert(ns->slots, new_cap, s);
    }
    ns->count = old->count;
    // Publish the new state.  Readers who dereference s_pid_ht after
    // this point see the new array; readers who dereferenced before
    // see the old one.
    rcu_assign_pointer(s_pid_ht, ns);
    // Defer reclamation of the old state to the caller (after lock drop).
    *out_old_for_reclaim = old;
    return ns;
}

// Reclaim an old pid_ht_state_t after a grow.  Must be called with the
// writer lock NOT held, because synchronize_rcu may yield.
static void pid_ht_reclaim_state(pid_ht_state_t* old) {
    if (!old) return;
    synchronize_rcu();
    kfree(old->slots);
    kfree(old);
}

void pid_ht_insert(task_t* t) {
    if (!t) return;
    pid_ht_state_t* old_for_reclaim = NULL;

    spin_lock(&s_pid_ht_lock);
    pid_ht_state_t* st = pid_ht_ensure_init_locked();
    if (!st) { spin_unlock(&s_pid_ht_lock); return; }
    // Grow at 75% load before inserting.
    if (st->count * 4u >= st->cap * 3u) {
        st = pid_ht_grow_locked(st, &old_for_reclaim);
        if (!st) { spin_unlock(&s_pid_ht_lock); return; }
    }
    pid_ht_raw_insert(st->slots, st->cap, t);
    st->count++;
    spin_unlock(&s_pid_ht_lock);

    // Reclaim the old state outside the lock so synchronize_rcu can
    // yield safely.
    pid_ht_reclaim_state(old_for_reclaim);
}

void pid_ht_remove(task_t* t) {
    if (!t) return;
    spin_lock(&s_pid_ht_lock);
    pid_ht_state_t* st = s_pid_ht;
    if (!st) { spin_unlock(&s_pid_ht_lock); return; }
    uint32_t i = pid_hash(t->pid, st->cap);
    for (uint32_t n = 0; n < st->cap; n++) {
        task_t* s = st->slots[i];
        if (!s) break;                          // empty — not found
        if (s == t) {
            // Leave a tombstone so concurrent readers stepping through
            // this probe chain see a "skipped" slot, not an empty one
            // (which would falsely signal "not found" for subsequent
            // entries in the chain).  The tombstone pointer is never
            // dereferenced.
            atomic_store_rel((task_t* volatile*)&st->slots[i], PID_HT_TOMB);
            st->count--;
            break;
        }
        i = (i + 1u) & (st->cap - 1u);
    }
    spin_unlock(&s_pid_ht_lock);
}

task_t* pid_ht_find(uint32_t pid) {
    rcu_read_lock();
    pid_ht_state_t* st = rcu_dereference(s_pid_ht);
    if (!st) { rcu_read_unlock(); return NULL; }
    uint32_t cap = st->cap;
    uint32_t i   = pid_hash(pid, cap);
    task_t* result = NULL;
    for (uint32_t n = 0; n < cap; n++) {
        task_t* s = atomic_load_acq(&st->slots[i]);
        if (!s) break;                          // empty — not found
        if (s != PID_HT_TOMB && s->pid == pid) { result = s; break; }
        i = (i + 1u) & (cap - 1u);
    }
    rcu_read_unlock();
    return result;
}

// Per-level FIFO queues.
// Per-CPU run queue / sleep list / zombie list all live in cpu_t.rq,
// protected by cpu_t.rq_lock.  See kernel/include/cpu.h.
//
// Under single CPU: all tasks have home_cpu==0 and live in g_cpus[0].
// Under SMP: each task lives on its home CPU's lists.
//
// _Static_assert keeps the MLFQ_LEVELS macro in sched.c in sync with
// the SCHED_MLFQ_LEVELS macro that sizes cpu_t.rq.heads[].
_Static_assert(MLFQ_LEVELS == SCHED_MLFQ_LEVELS,
               "MLFQ_LEVELS must match SCHED_MLFQ_LEVELS");

// ── Task index hash tables (pgid / tgid / sid → task list) ──────────────
// Three open-addressing hash tables keyed on group IDs.  Each slot holds
// a doubly-linked list of tasks sharing that ID, chained through the
// pg_prev/pg_next (or tg_*, sid_*) pointers in task_t.
//
// Used to make signal_send_pgrp / signal_send_group / tty_get_ctty O(list
// length in the group) instead of O(total tasks in system).
//
// Locking (SMP): today these run under the implicit UP lock; when SMP
// arrives each table gains a spinlock_t lock field protecting insert/
// remove/iterate.
//
// Each table stores a head pointer per bucket.  NULL = empty bucket.
// Deletes use tombstones only for *slots whose head is NULL*; since we
// delete by unlinking from the per-bucket list, we can keep the slot
// marker "alive" if other tasks still hash to the same bucket — but
// that's handled naturally by leaving the bucket head pointing at the
// remaining chain.  When the last task leaves a bucket, we set it to
// a tombstone to preserve probe chains for other IDs that collided here.

#define TIDX_INIT_CAP 32u
#define TIDX_TOMB     ((task_t*)1uL)

typedef struct {
    task_t**  slots;
    uint32_t  cap;
    uint32_t  count;      // number of non-empty buckets (not tasks)
} task_idx_t;

static task_idx_t s_pgid_ht;
static task_idx_t s_tgid_ht;
static task_idx_t s_sid_ht;

static uint32_t tidx_hash(uint32_t key, uint32_t cap) {
    return (key * 2654435761u) & (cap - 1u);
}

// Offset into task_t of {pg,tg,sid}_prev pointers.  We use a pair of
// functions per-table rather than offsets to keep the code legible.

// Ensure table has a slots array.
static void tidx_ensure_init(task_idx_t* ht) {
    if (ht->slots) return;
    ht->slots = (task_t**)kmalloc((uint64_t)TIDX_INIT_CAP * sizeof(task_t*));
    if (!ht->slots) return;
    __builtin_memset(ht->slots, 0, (uint64_t)TIDX_INIT_CAP * sizeof(task_t*));
    ht->cap = TIDX_INIT_CAP;
}

// Find bucket index for `key`.  Returns ht->cap if not present.
// A bucket is "present" iff its head slot is neither NULL nor TIDX_TOMB
// AND the head task's relevant ID equals `key`.
//
// We need table-specific access to read the head task's ID, so the caller
// passes a getter.
typedef uint32_t (*tidx_keyof_t)(const task_t*);

static uint32_t tidx_find(const task_idx_t* ht, uint32_t key, tidx_keyof_t keyof) {
    if (!ht->slots) return ht->cap;
    uint32_t i = tidx_hash(key, ht->cap);
    for (uint32_t n = 0; n < ht->cap; n++) {
        task_t* head = ht->slots[i];
        if (!head) return ht->cap;               // empty — not found
        if (head != TIDX_TOMB && keyof(head) == key) return i;
        i = (i + 1u) & (ht->cap - 1u);
    }
    return ht->cap;
}

// Find an insertion slot for `key` — first empty/tomb along the probe chain.
// Caller has already verified that `key` is not already present.
static uint32_t tidx_alloc_slot(task_idx_t* ht, uint32_t key) {
    uint32_t i = tidx_hash(key, ht->cap);
    for (uint32_t n = 0; n < ht->cap; n++) {
        task_t* s = ht->slots[i];
        if (!s || s == TIDX_TOMB) return i;
        i = (i + 1u) & (ht->cap - 1u);
    }
    return ht->cap; // full — caller must grow
}

static int tidx_grow(task_idx_t* ht, tidx_keyof_t keyof) {
    uint32_t new_cap = ht->cap * 2u;
    task_t** ns = (task_t**)kmalloc((uint64_t)new_cap * sizeof(task_t*));
    if (!ns) return -1;
    __builtin_memset(ns, 0, (uint64_t)new_cap * sizeof(task_t*));
    for (uint32_t i = 0; i < ht->cap; i++) {
        task_t* head = ht->slots[i];
        if (!head || head == TIDX_TOMB) continue;
        uint32_t key = keyof(head);
        uint32_t j = tidx_hash(key, new_cap);
        for (;;) {
            if (!ns[j]) { ns[j] = head; break; }
            j = (j + 1u) & (new_cap - 1u);
        }
    }
    kfree(ht->slots);
    ht->slots = ns;
    ht->cap   = new_cap;
    return 0;
}

// ── Per-table getters and link macros ───────────────────────────────────
static uint32_t keyof_pgid(const task_t* t) { return t->pgid; }
static uint32_t keyof_tgid(const task_t* t) { return t->tgid; }
static uint32_t keyof_sid (const task_t* t) { return t->sid;  }

// Generic doubly-linked-list insert at head of the bucket.
// The offsets of {prev,next} pointers are passed via accessor lambdas.
#define TIDX_DEFINE(name, prev_field, next_field, keyof)                      \
static void name##_insert(task_t* t) {                                        \
    tidx_ensure_init(&s_##name);                                              \
    if (!s_##name.slots) return;                                              \
    if (s_##name.count * 4u >= s_##name.cap * 3u)                             \
        if (tidx_grow(&s_##name, keyof) < 0) return;                          \
    uint32_t key = keyof(t);                                                  \
    uint32_t idx = tidx_find(&s_##name, key, keyof);                          \
    if (idx < s_##name.cap) {                                                 \
        /* existing bucket — link at head */                                  \
        task_t* old = s_##name.slots[idx];                                    \
        t->prev_field = NULL;                                                 \
        t->next_field = old;                                                  \
        if (old) old->prev_field = t;                                         \
        s_##name.slots[idx] = t;                                              \
    } else {                                                                  \
        /* new bucket */                                                      \
        uint32_t slot = tidx_alloc_slot(&s_##name, key);                      \
        if (slot >= s_##name.cap) return;                                     \
        t->prev_field = NULL;                                                 \
        t->next_field = NULL;                                                 \
        s_##name.slots[slot] = t;                                             \
        s_##name.count++;                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
static void name##_remove(task_t* t) {                                        \
    if (!s_##name.slots) return;                                              \
    uint32_t key = keyof(t);                                                  \
    uint32_t idx = tidx_find(&s_##name, key, keyof);                          \
    if (idx >= s_##name.cap) return;                                          \
    /* Unlink from doubly-linked list */                                      \
    task_t* prev = t->prev_field;                                             \
    task_t* next = t->next_field;                                             \
    if (prev) prev->next_field = next;                                        \
    if (next) next->prev_field = prev;                                        \
    if (s_##name.slots[idx] == t) {                                           \
        /* was head — replace with next, or tombstone if list now empty */    \
        s_##name.slots[idx] = next ? next : TIDX_TOMB;                        \
        if (!next) s_##name.count--;                                          \
    }                                                                         \
    t->prev_field = NULL;                                                     \
    t->next_field = NULL;                                                     \
}                                                                             \
                                                                              \
task_t* name##_head(uint32_t key) {                                           \
    if (!s_##name.slots) return NULL;                                         \
    uint32_t idx = tidx_find(&s_##name, key, keyof);                          \
    return (idx < s_##name.cap) ? s_##name.slots[idx] : NULL;                 \
}

TIDX_DEFINE(pgid_ht, pg_prev,  pg_next,  keyof_pgid)
TIDX_DEFINE(tgid_ht, tg_prev,  tg_next,  keyof_tgid)
TIDX_DEFINE(sid_ht,  sid_prev, sid_next, keyof_sid)

// Public entry points (declared in sched.h).
void task_idx_insert(task_t* t) {
    if (!t) return;
    pgid_ht_insert(t);
    tgid_ht_insert(t);
    sid_ht_insert(t);
}

void task_idx_remove(task_t* t) {
    if (!t) return;
    pgid_ht_remove(t);
    tgid_ht_remove(t);
    sid_ht_remove(t);
}

// Used by sys_setpgid to update membership when a task's pgid changes.
// The caller must have already updated t->pgid to the new value AFTER the
// remove — otherwise we can't find the old bucket.  So the correct
// sequence is: task_idx_pgid_changing(t); t->pgid = new; task_idx_pgid_changed(t);
void task_idx_pgid_changing(task_t* t) { pgid_ht_remove(t); }
void task_idx_pgid_changed (task_t* t) { pgid_ht_insert(t); }
void task_idx_sid_changing (task_t* t) { sid_ht_remove(t);  }
void task_idx_sid_changed  (task_t* t) { sid_ht_insert(t);  }

task_t* task_idx_pgid_head(uint32_t pgid) { return pgid_ht_head(pgid); }
task_t* task_idx_tgid_head(uint32_t tgid) { return tgid_ht_head(tgid); }
task_t* task_idx_sid_head (uint32_t sid)  { return sid_ht_head(sid);   }

// Idle process — never put on a queue, runs when all queues are empty.
static uint8_t      s_idle_stack[PAGE_SIZE];
static task_mm_t    s_idle_mm    = { .pml4_phys = 0, .mm = NULL, .refs = 1 };
static task_files_t s_idle_files = { .fd_table = NULL, .fd_capacity = 0, .refs = 1 };
static task_t s_idle = {
    .pid          = 0,
    .tgid         = 0,
    .ppid         = 0,
    .flags        = TASK_FLAG_KTHREAD,
    .state        = TASK_RUNNING,
    .mm_shared    = &s_idle_mm,
    .files_shared = &s_idle_files,
    .ctx          = {0},
    .kstack_top   = (uint64_t)s_idle_stack + PAGE_SIZE,
    .next         = NULL,
};

static volatile uint32_t s_tick_count  = 0;

// ── Internal helpers ──────────────────────────────────────────────────────
//
// Every helper below takes a cpu_rq_t* (pointing at the owning CPU's rq)
// so the same code works for any CPU.  Callers must hold that CPU's
// rq_lock.

// Enqueue at the task's current mlfq_level.  Caller holds target's rq_lock.
static void enqueue_on(cpu_rq_t* rq, task_t* p) {
    p->state = TASK_READY;
    p->next  = NULL;
    if (p->mlfq_ticks_left == 0)
        p->mlfq_ticks_left = s_quanta[p->mlfq_level];

    uint8_t lvl = p->mlfq_level;
    if (!rq->tails[lvl]) {
        rq->heads[lvl] = rq->tails[lvl] = p;
    } else {
        rq->tails[lvl]->next = p;
        rq->tails[lvl] = p;
    }
}

// Dequeue from the highest non-empty level.  Caller holds this CPU's rq_lock.
static task_t* dequeue_local(cpu_rq_t* rq) {
    for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
        if (!rq->heads[i]) continue;
        task_t* p = rq->heads[i];
        rq->heads[i] = p->next;
        if (!rq->heads[i]) rq->tails[i] = NULL;
        p->next = NULL;
        return p;
    }
    return NULL;
}

// Pick the target CPU for a new task.  Under single CPU, always 0.
// Phase 9 will add a real placement policy (least-loaded, cache affinity).
static inline uint32_t pick_home_cpu(void) {
    return 0;
}

// Shortcut to the owning CPU's rq.
static inline cpu_t* cpu_of_task(task_t* t) {
    return &g_cpus[t->home_cpu];
}

// ── Public API ────────────────────────────────────────────────────────────

void sched_init(void) {
    // Per-CPU run queues are BSS-zeroed; rq_lock was initialised by
    // cpu_init_bsp.  Nothing to do here for the queues themselves.
    // g_current expands to (this_cpu()->current) via sched.h macro, so
    // assigning to it stores into the per-CPU slot directly.
    this_cpu()->current = &s_idle;
    this_cpu()->idle    = &s_idle;
    timer_register_tick(sched_tick);
}

void sched_add(task_t* proc) {
    // Register the first user process as the init/orphan-reaper task.
    if (!g_init_task && !(proc->flags & TASK_FLAG_KTHREAD))
        g_init_task = proc;

    // Assign a home CPU once; the task stays there until Phase 9 adds
    // work stealing.  On single CPU this is always 0.
    proc->home_cpu = pick_home_cpu();

    pid_ht_insert(proc);
    task_idx_insert(proc);

    cpu_t* c = &g_cpus[proc->home_cpu];
    uint64_t flags = spin_lock_irqsave(&c->rq_lock);
    enqueue_on(&c->rq, proc);
    spin_unlock_irqrestore(&c->rq_lock, flags);
}

// ── Per-task child list ───────────────────────────────────────────────────

void task_child_add(task_t* parent, task_t* child) {
    child->child_next  = parent->children;
    parent->children   = child;
}

void task_child_remove(task_t* parent, task_t* child) {
    task_t** pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->child_next;
            child->child_next = NULL;
            return;
        }
        pp = &(*pp)->child_next;
    }
}

// ── sched_tick ────────────────────────────────────────────────────────────
// Called from timer IRQ — only sets flags/counters, never touches the stack.

void sched_tick(void) {
    s_tick_count++;

    // sched_tick runs on THIS CPU — it's the local timer ISR.  Only
    // touch this CPU's rq.
    cpu_t*    c  = this_cpu();
    cpu_rq_t* rq = &c->rq;

    uint64_t flags = spin_lock_irqsave(&c->rq_lock);

    // Wake any sleeping tasks whose deadline has passed.
    uint64_t now = tsc_read_ns();
    task_t** pp = &rq->sleep_head;
    while (*pp) {
        task_t* t = *pp;
        if (t->sleep_until_ns && now >= t->sleep_until_ns) {
            *pp = t->next;
            t->next = NULL;
            t->sleep_until_ns = 0;
            enqueue_on(rq, t);
        } else {
            pp = &t->next;
        }
    }

    // Priority boost: move every task in levels 1-3 back to level 0.
    if (s_tick_count % BOOST_INTERVAL == 0) {
        for (uint8_t i = 1; i < MLFQ_LEVELS; i++) {
            task_t* t = rq->heads[i];
            while (t) {
                task_t* nxt = t->next;
                t->mlfq_level     = 0;
                t->mlfq_ticks_left = s_quanta[0];
                t->next = NULL;
                if (!rq->tails[0]) { rq->heads[0] = rq->tails[0] = t; }
                else               { rq->tails[0]->next = t; rq->tails[0] = t; }
                t = nxt;
            }
            rq->heads[i] = rq->tails[i] = NULL;
        }
        // Boost the currently running task too.
        if (c->current && c->current != c->idle) {
            c->current->mlfq_level      = 0;
            c->current->mlfq_ticks_left = s_quanta[0];
        }
    }

    // Tick down the running task's quantum.
    if (c->current && c->current != c->idle) {
        if (c->current->mlfq_ticks_left > 0)
            c->current->mlfq_ticks_left--;
        if (c->current->mlfq_ticks_left == 0)
            c->reschedule_pending = 1;
    } else {
        c->reschedule_pending = 1;   // idle → always try to find real work
    }

    spin_unlock_irqrestore(&c->rq_lock, flags);
}

// ── do_switch ─────────────────────────────────────────────────────────────
// Called from process context.  `preempted` is true when the quantum expired
// (timer path); false for voluntary yield/sleep.

static void do_switch(uint8_t preempted) {
    cpu_t*    c  = this_cpu();
    cpu_rq_t* rq = &c->rq;
    task_t*   next;

    // Pick the next runnable task under the local rq_lock.  If nothing
    // is ready and this is a voluntary yield/sleep, halt until an IRQ
    // wakes something — releasing the lock across the hlt so wakers
    // on other CPUs (or our own IRQ path) can actually enqueue work.
    uint64_t flags = spin_lock_irqsave(&c->rq_lock);
    next = dequeue_local(rq);
    if (!next) {
        if (preempted) {
            // Nothing to switch to — let the current task keep running.
            spin_unlock_irqrestore(&c->rq_lock, flags);
            return;
        }
        // Voluntary yield/sleep with an empty runqueue: drop the lock,
        // halt until an interrupt, re-take and re-dequeue.
        for (;;) {
            spin_unlock_irqrestore(&c->rq_lock, flags);
            rcu_note_qs();
            __asm__ volatile("sti\nhlt\ncli");
            flags = spin_lock_irqsave(&c->rq_lock);
            next = dequeue_local(rq);
            if (next) break;
        }
    }

    task_t* prev = c->current;
    if (prev != c->idle && prev->state == TASK_RUNNING) {
        if (preempted) {
            // Timer preemption — quantum expired, demote.
            if (prev->mlfq_level < MLFQ_LEVELS - 1)
                prev->mlfq_level++;
            prev->mlfq_ticks_left = s_quanta[prev->mlfq_level];
        } else {
            // Voluntary yield — charge one tick so spin-yielders get
            // demoted.  I/O tasks use sched_sleep (which refreshes the
            // quantum), so they keep their priority.
            if (prev->mlfq_ticks_left > 0)
                prev->mlfq_ticks_left--;
            if (prev->mlfq_ticks_left == 0) {
                if (prev->mlfq_level < MLFQ_LEVELS - 1)
                    prev->mlfq_level++;
                prev->mlfq_ticks_left = s_quanta[prev->mlfq_level];
            }
        }
        enqueue_on(rq, prev);
    }

    next->state = TASK_RUNNING;
    c->current  = next;     // g_current expands to this via the sched.h macro
    c->context_switches++;

    // Release the rq_lock BEFORE context_switch.  Holding it across
    // the switch would deadlock: the new task might try to take the
    // same lock before the old task's unlock ever runs.
    spin_unlock_irqrestore(&c->rq_lock, flags);

    // Every context switch is an RCU quiescent state for this CPU: the
    // outgoing task can no longer be in a reader section, and the
    // incoming task starts fresh.  Bumping the counter here lets
    // synchronize_rcu() observe that this CPU has made progress.
    rcu_note_qs();

    tss_set_rsp0(next->kstack_top);
    context_switch(&prev->ctx, &next->ctx, next->mm_shared->pml4_phys);

    signal_deliver_pending();

    if (prev != c->idle && prev->state == TASK_DEAD)
        process_destroy(prev);

    __asm__ volatile("sti");
}

// Forward declaration — body below sched_sleep.
static NOINLINE void sched_sleep_while_preempt_disabled_panic(void);

void sched_yield(void) {
    // Same invariant as sched_sleep: yielding with preempt disabled is
    // a bug that would leak the preempt state across the context switch.
    if (UNLIKELY(this_cpu()->preempt_depth > 0))
        sched_sleep_while_preempt_disabled_panic();
    this_cpu()->reschedule_pending = 0;
    do_switch(0);
}

void sched_preempt(void) {
    cpu_t* c = this_cpu();
    if (!c->reschedule_pending) return;
    // Honour preemption disable: defer the switch until depth reaches zero.
    // The preempt_enable() path will call sched_preempt() again then.
    if (c->preempt_depth > 0) return;
    c->reschedule_pending = 0;
    do_switch(1);
}

// Sleeping with preemption disabled is a bug that always leads to
// deadlock (the CPU can't context-switch away, so the sleep never
// returns).  Catch it loudly instead of silently hanging.
static NOINLINE void sched_sleep_while_preempt_disabled_panic(void) {
    serial_puts_dbg("[sched] PANIC: sched_sleep with preempt_depth > 0\n");
    serial_puts_dbg("[sched]   pid=");
    serial_hex_dbg(g_current ? (uint64_t)g_current->pid : 0);
    serial_puts_dbg("[sched]   depth=");
    serial_hex_dbg((uint64_t)this_cpu()->preempt_depth);
    for (;;) __asm__ volatile("cli; hlt");
}

void sched_sleep(void) {
    // Enforce: no task may sleep while preempt is disabled.  This is the
    // single runtime check that makes preempt_disable safe everywhere —
    // RCU readers, INITCALL_FLAG_PREEMPT_OFF, fb_term_putc, slab fast
    // paths (Phase 4), anywhere.  Violation = immediate panic with enough
    // context for a post-mortem.
    cpu_t* c = this_cpu();
    if (UNLIKELY(c->preempt_depth > 0))
        sched_sleep_while_preempt_disabled_panic();

    if (g_current && g_current != c->idle) {
        // Put ourselves on our home CPU's sleep list under its rq_lock.
        // On UP that's always this CPU, so we just need the local lock.
        task_t* self = g_current;
        cpu_t* home  = cpu_of_task(self);
        uint64_t flags = spin_lock_irqsave(&home->rq_lock);
        self->state = TASK_SLEEPING;
        // Refresh quantum so the task gets a full slice when woken.
        self->mlfq_ticks_left = s_quanta[self->mlfq_level];
        self->next = home->rq.sleep_head;
        home->rq.sleep_head = self;
        spin_unlock_irqrestore(&home->rq_lock, flags);
    }
    c->reschedule_pending = 0;
    do_switch(0);
}

void sched_wake(task_t* proc) {
    if (!proc) return;
    // Touch target CPU's rq under its lock.  Cross-CPU safe: we grab
    // the owning CPU's rq_lock before looking at anything.
    cpu_t* home = cpu_of_task(proc);
    uint64_t flags = spin_lock_irqsave(&home->rq_lock);
    if (proc->state != TASK_SLEEPING) {
        spin_unlock_irqrestore(&home->rq_lock, flags);
        return;
    }
    // Remove from the target CPU's sleep list.
    task_t** pp = &home->rq.sleep_head;
    while (*pp && *pp != proc) pp = &(*pp)->next;
    if (*pp) *pp = proc->next;
    proc->next = NULL;
    enqueue_on(&home->rq, proc);

    // If the woken task has strictly higher priority than the currently
    // running task on its home CPU, ask that CPU to reschedule.
    task_t* cur = home->current;
    if (cur && cur != home->idle && cur->mlfq_level > 0 &&
        proc->mlfq_level < cur->mlfq_level) {
        home->reschedule_pending = 1;
        // TODO Phase 9: if home != this_cpu, send an IPI so the target
        // CPU actually context-switches soon instead of waiting for its
        // next tick.
    }
    spin_unlock_irqrestore(&home->rq_lock, flags);
}

// Add a TASK_ZOMBIE task to its home CPU's zombie list.
//
// Zombies STAY in the pid hash table (s_pid_ht).  This keeps every
// specific-pid lookup O(1) — waitpid(pid), kill(pid), /proc/[pid],
// setpgid(pid) — regardless of whether the task is alive or a zombie.
// Only task_destroy (the final reap) removes the entry from pid_ht.
//
// Zombies ARE removed from the task_idx (pgid/tgid/sid) tables so
// that broadcast signal delivery (signal_send_pgrp, signal_send_group)
// skips them.
void sched_add_zombie(task_t* t) {
    task_idx_remove(t);
    cpu_t* home = cpu_of_task(t);
    uint64_t flags = spin_lock_irqsave(&home->rq_lock);
    t->next = home->rq.zombie_head;
    home->rq.zombie_head = t;
    spin_unlock_irqrestore(&home->rq_lock, flags);
}

// Unlink a known zombie from its home CPU's zombie list.
// Returns 1 if unlinked, 0 if not found (already reaped).
// Caller must not hold any rq_lock.
static int zombie_unlink(task_t* z) {
    if (!z) return 0;
    cpu_t* home = cpu_of_task(z);
    uint64_t flags = spin_lock_irqsave(&home->rq_lock);
    task_t** pp = &home->rq.zombie_head;
    while (*pp && *pp != z) pp = &(*pp)->next;
    if (!*pp) {
        spin_unlock_irqrestore(&home->rq_lock, flags);
        return 0;
    }
    *pp = z->next;
    z->next = NULL;
    spin_unlock_irqrestore(&home->rq_lock, flags);
    return 1;
}

// Remove and return the zombie with the given pid.  O(1) when pid != 0.
// pid == 0 is an "any zombie" query that still needs a per-CPU walk —
// but that's a cold path used by kthread cleanup / orphan reap, so the
// extra cost is fine.
task_t* sched_reap_zombie(uint32_t pid) {
    if (pid != 0) {
        // O(1): hash lookup, state check, unlink from home CPU list.
        task_t* z = pid_ht_find(pid);
        if (!z || z->state != TASK_ZOMBIE) return NULL;
        if (!zombie_unlink(z)) return NULL;
        return z;
    }
    // pid == 0: find ANY zombie anywhere.
    unsigned n = num_cpus();
    for (unsigned ci = 0; ci < n; ci++) {
        cpu_t* c = &g_cpus[ci];
        uint64_t flags = spin_lock_irqsave(&c->rq_lock);
        task_t* z = c->rq.zombie_head;
        if (z) {
            c->rq.zombie_head = z->next;
            z->next = NULL;
            spin_unlock_irqrestore(&c->rq_lock, flags);
            return z;
        }
        spin_unlock_irqrestore(&c->rq_lock, flags);
    }
    return NULL;
}

// Remove and return a zombie that is a child of parent_pid.  O(1) when
// target_pid != 0; O(children) when target_pid == 0 (walk parent's
// children list — typically very small).
task_t* sched_reap_child_zombie(uint32_t parent_pid, uint32_t target_pid) {
    if (target_pid != 0) {
        // O(1): look up the specific pid, verify parent + zombie state.
        task_t* z = pid_ht_find(target_pid);
        if (!z || z->state != TASK_ZOMBIE || z->ppid != parent_pid)
            return NULL;
        if (!zombie_unlink(z)) return NULL;
        return z;
    }
    // target_pid == 0: find ANY zombie child of parent_pid.  Walk the
    // parent's own children list (maintained as task_t.children /
    // child_next) rather than the per-CPU zombie lists — children is
    // typically tiny and it keeps us away from cross-CPU walks.
    task_t* parent = pid_ht_find(parent_pid);
    if (!parent) return NULL;
    for (task_t* c = parent->children; c; c = c->child_next) {
        if (c->state == TASK_ZOMBIE) {
            if (zombie_unlink(c)) return c;
        }
    }
    return NULL;
}

// Check if any zombie child of parent_pid exists (non-blocking poll).
// O(1) when target_pid != 0; O(children) otherwise.
uint8_t sched_has_child_zombie(uint32_t parent_pid, uint32_t target_pid) {
    if (target_pid != 0) {
        task_t* z = pid_ht_find(target_pid);
        return (z && z->state == TASK_ZOMBIE && z->ppid == parent_pid) ? 1 : 0;
    }
    task_t* parent = pid_ht_find(parent_pid);
    if (!parent) return 0;
    for (task_t* c = parent->children; c; c = c->child_next)
        if (c->state == TASK_ZOMBIE) return 1;
    return 0;
}

// ── sched_wait_pid ────────────────────────────────────────────────────────

// Returns 1 if a matching zombie was found OR the specific pid is
// gone from the system entirely; 0 if it's still alive and we should
// keep waiting.
//
// pid != 0: O(1) per retry via pid_ht_find.
// pid == 0: O(children) per retry (walk parent's children list).
uint8_t sched_wait_pid(uint32_t pid) {
    for (;;) {
        if (pid != 0) {
            // O(1): look up the specific pid.  If it's gone, waitpid
            // returns "reaped"; if it's a zombie, waitpid will reap it
            // via sched_reap_*; otherwise keep waiting.
            task_t* t = pid_ht_find(pid);
            if (!t) return 1;                 // fully gone already
            if (t->state == TASK_ZOMBIE) return 1;
        } else {
            // "any child" — check the current task's children list.
            if (g_current) {
                for (task_t* c = g_current->children; c; c = c->child_next)
                    if (c->state == TASK_ZOMBIE) return 1;
                if (!g_current->children) return 1; // no children at all
            }
        }
        sched_yield();
    }
}

// Non-blocking variant.  O(1) when pid != 0.
uint8_t sched_poll_pid(uint32_t pid) {
    if (pid != 0) {
        task_t* t = pid_ht_find(pid);
        return (t && t->state == TASK_ZOMBIE) ? 1 : 0;
    }
    // pid == 0: check the current task's children list for any zombie.
    if (!g_current) return 0;
    for (task_t* c = g_current->children; c; c = c->child_next)
        if (c->state == TASK_ZOMBIE) return 1;
    return 0;
}

// ── sched_for_each ────────────────────────────────────────────────────────
// Walks every task on every CPU's runqueue/sleep/zombie lists.  Cold
// path — used by /proc/[pid] enumeration and broadcast kill(-1).  Takes
// each CPU's rq_lock in turn.
//
// WARNING: the callback must NOT take any lock that could be held by
// the scheduler (or take g_pmm_lock, etc.).  Nested lock acquisition
// through this path is a deadlock surface.  All existing callers pass
// trivial visitor functions that just read task fields.

void sched_for_each(void (*cb)(task_t*, void*), void* data) {
    // The currently running task on THIS CPU isn't in any list — it
    // was dequeued before it started running — so include it
    // explicitly.  Other CPUs' current tasks are handled below by
    // iterating their cpu_t.current.
    if (g_current && g_current != this_cpu()->idle)
        cb(g_current, data);

    unsigned n = num_cpus();
    for (unsigned ci = 0; ci < n; ci++) {
        cpu_t* c = &g_cpus[ci];

        // Visit that CPU's currently running task too (if it's not us).
        if (c != this_cpu() && c->current && c->current != c->idle)
            cb(c->current, data);

        uint64_t flags = spin_lock_irqsave(&c->rq_lock);
        for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
            task_t* t = c->rq.heads[i];
            while (t) { cb(t, data); t = t->next; }
        }
        task_t* t = c->rq.sleep_head;
        while (t) { cb(t, data); t = t->next; }
        t = c->rq.zombie_head;
        while (t) { cb(t, data); t = t->next; }
        spin_unlock_irqrestore(&c->rq_lock, flags);
    }
}

// ── sched_find_pid ────────────────────────────────────────────────────────
// O(1) lookup via pid hash table.
task_t* sched_find_pid(uint32_t pid) {
    return pid_ht_find(pid);
}

// ── sched_queue_head — removed: use sched_for_each instead ───────────────
