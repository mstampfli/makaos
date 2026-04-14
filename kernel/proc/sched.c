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

task_t* g_current   = NULL;
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

// Grow the hash table.  Publishes the new state, then synchronize_rcu
// before freeing the old one.
static int pid_ht_grow_locked(pid_ht_state_t* old) {
    uint32_t new_cap = old->cap * 2u;
    pid_ht_state_t* ns = pid_ht_alloc_state(new_cap);
    if (!ns) return -1;
    // Copy all live entries (skip tombstones).
    for (uint32_t i = 0; i < old->cap; i++) {
        task_t* s = old->slots[i];
        if (s && s != PID_HT_TOMB) pid_ht_raw_insert(ns->slots, new_cap, s);
    }
    ns->count = old->count;
    // Publish the new state.  Readers who dereference s_pid_ht after
    // this point see the new array; readers who dereferenced before see
    // the old one.
    rcu_assign_pointer(s_pid_ht, ns);
    // Wait for any in-flight readers on the old state to finish, then
    // reclaim it.  synchronize_rcu is a no-op on UP.
    synchronize_rcu();
    kfree(old->slots);
    kfree(old);
    return 0;
}

void pid_ht_insert(task_t* t) {
    if (!t) return;
    spin_lock(&s_pid_ht_lock);
    pid_ht_state_t* st = pid_ht_ensure_init_locked();
    if (!st) { spin_unlock(&s_pid_ht_lock); return; }
    // Grow at 75% load before inserting.
    if (st->count * 4u >= st->cap * 3u) {
        if (pid_ht_grow_locked(st) < 0) { spin_unlock(&s_pid_ht_lock); return; }
        st = s_pid_ht;
    }
    pid_ht_raw_insert(st->slots, st->cap, t);
    st->count++;
    spin_unlock(&s_pid_ht_lock);
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
static task_t* s_heads[MLFQ_LEVELS];
static task_t* s_tails[MLFQ_LEVELS];

// Singly-linked list of sleeping tasks (state == TASK_SLEEPING).
// Uses task_t::next as the link field.
static task_t* s_sleep_head = NULL;

// Singly-linked list of zombie tasks (state == TASK_ZOMBIE).
// Tasks stay here until the parent calls wait() to reap them.
static task_t* s_zombie_head = NULL;

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
static volatile uint8_t  s_reschedule  = 0;

// ── Internal helpers ──────────────────────────────────────────────────────

// Enqueue at the task's current mlfq_level.
// Initialises ticks_left if this is the task's first run (ticks_left == 0).
static void enqueue(task_t* p) {
    p->state = TASK_READY;
    p->next  = NULL;
    if (p->mlfq_ticks_left == 0)
        p->mlfq_ticks_left = s_quanta[p->mlfq_level];

    uint8_t lvl = p->mlfq_level;
    if (!s_tails[lvl]) {
        s_heads[lvl] = s_tails[lvl] = p;
    } else {
        s_tails[lvl]->next = p;
        s_tails[lvl] = p;
    }
}

// Dequeue from the highest non-empty level.
static task_t* dequeue(void) {
    for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
        if (!s_heads[i]) continue;
        task_t* p = s_heads[i];
        s_heads[i] = p->next;
        if (!s_heads[i]) s_tails[i] = NULL;
        p->next = NULL;
        return p;
    }
    return NULL;
}

// ── Public API ────────────────────────────────────────────────────────────

void sched_init(void) {
    for (uint8_t i = 0; i < MLFQ_LEVELS; i++)
        s_heads[i] = s_tails[i] = NULL;

    g_current = &s_idle;
    // Mirror into the per-CPU slot so this_cpu()->current is always in
    // sync.  Phase 5 will make this the primary storage and demote
    // g_current to a compatibility macro.
    this_cpu()->current = &s_idle;
    this_cpu()->idle    = &s_idle;
    timer_register_tick(sched_tick);
}

void sched_add(task_t* proc) {
    // Register the first user process as the init/orphan-reaper task.
    if (!g_init_task && !(proc->flags & TASK_FLAG_KTHREAD))
        g_init_task = proc;
    pid_ht_insert(proc);
    task_idx_insert(proc);
    enqueue(proc);
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

    // Wake any sleeping tasks whose deadline has passed.
    uint64_t now = tsc_read_ns();
    task_t** pp = &s_sleep_head;
    while (*pp) {
        task_t* t = *pp;
        if (t->sleep_until_ns && now >= t->sleep_until_ns) {
            *pp = t->next;
            t->next = NULL;
            t->sleep_until_ns = 0;
            enqueue(t);
        } else {
            pp = &t->next;
        }
    }

    // Priority boost: move every task in levels 1-3 back to level 0.
    if (s_tick_count % BOOST_INTERVAL == 0) {
        for (uint8_t i = 1; i < MLFQ_LEVELS; i++) {
            task_t* t = s_heads[i];
            while (t) {
                task_t* nxt = t->next;
                t->mlfq_level     = 0;
                t->mlfq_ticks_left = s_quanta[0];
                t->next = NULL;
                if (!s_tails[0]) { s_heads[0] = s_tails[0] = t; }
                else             { s_tails[0]->next = t; s_tails[0] = t; }
                t = nxt;
            }
            s_heads[i] = s_tails[i] = NULL;
        }
        // Boost the currently running task too.
        if (g_current && g_current != &s_idle) {
            g_current->mlfq_level      = 0;
            g_current->mlfq_ticks_left = s_quanta[0];
        }
    }

    // Tick down the running task's quantum.
    if (g_current && g_current != &s_idle) {
        if (g_current->mlfq_ticks_left > 0)
            g_current->mlfq_ticks_left--;
        if (g_current->mlfq_ticks_left == 0)
            s_reschedule = 1;
    } else {
        s_reschedule = 1;   // idle → always try to find real work
    }
}

// ── do_switch ─────────────────────────────────────────────────────────────
// Called from process context.  `preempted` is true when the quantum expired
// (timer path); false for voluntary yield/sleep.

static void do_switch(uint8_t preempted) {
    task_t* next = dequeue();

    if (!next) {
        if (preempted) return;   // nothing to switch to — let current task keep running
        // Voluntary yield/sleep: HLT until an interrupt wakes something.
        // Every hlt wakeup is an RCU quiescent state: we can't possibly
        // be inside a reader section while halted.
        while (!next) {
            rcu_note_qs();
            __asm__ volatile("sti\nhlt\ncli");
            next = dequeue();
        }
    }

    task_t* prev = g_current;
    if (prev != &s_idle && prev->state == TASK_RUNNING) {
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
        enqueue(prev);
    }

    next->state = TASK_RUNNING;
    g_current   = next;
    this_cpu()->current = next;
    this_cpu()->context_switches++;
    // Every context switch is an RCU quiescent state for this CPU: the
    // outgoing task can no longer be in a reader section, and the
    // incoming task starts fresh.  Bumping the counter here lets
    // synchronize_rcu() observe that this CPU has made progress.
    rcu_note_qs();

    tss_set_rsp0(next->kstack_top);
    context_switch(&prev->ctx, &next->ctx, next->mm_shared->pml4_phys);

    signal_deliver_pending();

    if (prev != &s_idle && prev->state == TASK_DEAD)
        process_destroy(prev);

    __asm__ volatile("sti");
}

void sched_yield(void) {
    s_reschedule = 0;
    do_switch(0);
}

void sched_preempt(void) {
    if (!s_reschedule) return;
    // Honour preemption disable: defer the switch until depth reaches zero.
    // The preempt_enable() path will call sched_preempt() again then.
    if (this_cpu()->preempt_depth > 0) return;
    s_reschedule = 0;
    do_switch(1);
}

void sched_sleep(void) {
    if (g_current && g_current != &s_idle) {
        g_current->state = TASK_SLEEPING;
        // Refresh quantum so the task gets a full slice when woken.
        g_current->mlfq_ticks_left = s_quanta[g_current->mlfq_level];
        // Add to sleeping list so sched_for_each can still find it.
        g_current->next = s_sleep_head;
        s_sleep_head = g_current;
    }
    s_reschedule = 0;
    do_switch(0);
}

void sched_wake(task_t* proc) {
    if (!proc) return;
    if (proc->state != TASK_SLEEPING) return;
    // Remove from sleeping list.
    task_t** pp = &s_sleep_head;
    while (*pp && *pp != proc) pp = &(*pp)->next;
    if (*pp) *pp = proc->next;
    proc->next = NULL;
    enqueue(proc);
    // If the woken task has strictly higher priority (lower level number)
    // than the currently running task, request a preemption.  I/O-bound
    // tasks stay at level 0 (sched_sleep refreshes their quantum), so this
    // ensures they preempt CPU-bound tasks that have been demoted.
    if (g_current != &s_idle && g_current->mlfq_level > 0 &&
        proc->mlfq_level < g_current->mlfq_level) {
        s_reschedule = 1;
    }
}

// Add a TASK_ZOMBIE task to the zombie list (called from sys_exit).
void sched_add_zombie(task_t* t) {
    pid_ht_remove(t);
    task_idx_remove(t);
    t->next = s_zombie_head;
    s_zombie_head = t;
}

// Remove and return the zombie with the given pid (0 = any).
// Does NOT check parent — use sched_reap_child_zombie for waitpid.
// Returns NULL if not found.  Caller must call process_destroy on result.
task_t* sched_reap_zombie(uint32_t pid) {
    task_t** pp = &s_zombie_head;
    while (*pp) {
        if (pid == 0 || (*pp)->pid == pid) {
            task_t* z = *pp;
            *pp = z->next;
            z->next = NULL;
            pid_ht_remove(z);
            return z;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

// Remove and return a zombie that is a child of `parent_pid`.
// If target_pid == 0: reaps any child of parent_pid.
// If target_pid != 0: reaps that specific pid only if its ppid == parent_pid.
// Returns NULL if no matching child zombie found.
task_t* sched_reap_child_zombie(uint32_t parent_pid, uint32_t target_pid) {
    task_t** pp = &s_zombie_head;
    while (*pp) {
        task_t* z = *pp;
        int pid_match  = (target_pid == 0) || (z->pid == target_pid);
        int is_child   = (z->ppid == parent_pid);
        if (pid_match && is_child) {
            *pp = z->next;
            z->next = NULL;
            pid_ht_remove(z);
            return z;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

// Check if any zombie child of parent_pid exists (non-blocking poll).
// If target_pid != 0, checks specifically for that pid.
uint8_t sched_has_child_zombie(uint32_t parent_pid, uint32_t target_pid) {
    task_t* z = s_zombie_head;
    while (z) {
        int pid_match = (target_pid == 0) || (z->pid == target_pid);
        int is_child  = (z->ppid == parent_pid);
        if (pid_match && is_child) return 1;
        z = z->next;
    }
    return 0;
}

// ── sched_wait_pid ────────────────────────────────────────────────────────

typedef struct { uint32_t pid; uint8_t found; } wait_pid_arg_t;

static void find_pid_cb(task_t* t, void* data) {
    wait_pid_arg_t* a = (wait_pid_arg_t*)data;
    if (t->pid == a->pid) a->found = 1;
}

// Returns 1 if a matching zombie was found (or task is gone), 0 if still alive.
// pid == 0 means "any child".
uint8_t sched_wait_pid(uint32_t pid) {
    for (;;) {
        // Check zombie list.
        task_t* z = s_zombie_head;
        while (z) {
            if (pid == 0 || z->pid == pid) return 1;
            z = z->next;
        }
        if (pid == 0) { sched_yield(); continue; } // any: keep waiting
        // Specific pid: check if still alive anywhere (run queue, sleep, zombie)
        // or is the currently running task.
        wait_pid_arg_t a = {pid, 0};
        __asm__ volatile("cli");
        sched_for_each(find_pid_cb, &a);
        __asm__ volatile("sti");
        if (!a.found) return 1;
        sched_yield();
    }
}

// Non-blocking variant: returns 1 if zombie ready, 0 if not.
uint8_t sched_poll_pid(uint32_t pid) {
    task_t* z = s_zombie_head;
    while (z) {
        if (pid == 0 || z->pid == pid) return 1;
        z = z->next;
    }
    return 0;
}

// ── sched_for_each ────────────────────────────────────────────────────────
// Walks every task in every MLFQ level.

void sched_for_each(void (*cb)(task_t*, void*), void* data) {
    // Include g_current: it is dequeued while executing, so it won't appear
    // in any of the lists below.  Callers (e.g. sched_wait_pid) need to see it.
    if (g_current && g_current != &s_idle)
        cb(g_current, data);

    for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
        task_t* t = s_heads[i];
        while (t) {
            cb(t, data);
            t = t->next;
        }
    }
    // Also walk sleeping and zombie tasks so sched_wait_pid doesn't miss them.
    task_t* t = s_sleep_head;
    while (t) { cb(t, data); t = t->next; }
    t = s_zombie_head;
    while (t) { cb(t, data); t = t->next; }
}

// ── sched_find_pid ────────────────────────────────────────────────────────
// O(1) lookup via pid hash table.
task_t* sched_find_pid(uint32_t pid) {
    return pid_ht_find(pid);
}

// ── sched_queue_head — removed: use sched_for_each instead ───────────────
