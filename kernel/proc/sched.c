#include "sched.h"
#include "process.h"
#include "signal.h"
#include "timer.h"
#include "tsc.h"
#include "tss.h"
#include "common.h"
#include "kheap.h"
#include "cpu.h"
#include "vmm.h"
#include "lapic.h"
#include "rcu.h"
#include "ahci.h"


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
// writer lock NOT held, because synchronize_rcu may yield.  Uses the
// expedited grace period — pid_ht_grow runs inside pid_ht_insert on
// the user-syscall path (every task spawn), so sub-ms latency here
// matters even though the grow itself is rare.
static void pid_ht_reclaim_state(pid_ht_state_t* old) {
    if (!old) return;
    synchronize_rcu_expedited();
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
    task_t**   slots;
    uint32_t   cap;
    uint32_t   count;     // number of non-empty buckets (not tasks)
    spinlock_t lock;      // IRQ-safe; guards slots, cap, count, and the
                          // pg/tg/sid linked-list pointers inside every
                          // task stored in this table.  Taken by find,
                          // insert, remove, and by the head walkers in
                          // signal_send_pgrp / signal_send_group /
                          // tty_get_ctty (via head accessors).
} task_idx_t;

static task_idx_t s_pgid_ht = { .slots = NULL, .cap = 0, .count = 0,
                                .lock = SPINLOCK_INIT };
static task_idx_t s_tgid_ht = { .slots = NULL, .cap = 0, .count = 0,
                                .lock = SPINLOCK_INIT };
static task_idx_t s_sid_ht  = { .slots = NULL, .cap = 0, .count = 0,
                                .lock = SPINLOCK_INIT };

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

// Each generated _insert / _remove takes the table's per-table
// spinlock via spin_lock_irqsave.  The lock covers the entire
// operation (probe, resize if needed, doubly-linked-list unlink /
// relink).  Critical section is bounded by the probe depth + the
// linked-list unlink — trivially small for realistic process groups.
//
// The generated _walk(key, cb, data) holds the lock for the whole
// walk so concurrent remove/insert can't tear the list.  The callback
// MUST NOT take any lock that the writers might be waiting on, and
// MUST be short.  It may call signal_send (which takes rq_lock of
// the target task's home CPU — a strictly-lower lock in the ordering
// pgid_ht.lock > rq_lock, so no cycle).
#define TIDX_DEFINE(name, prev_field, next_field, keyof)                      \
static void name##_insert(task_t* t) {                                        \
    uint64_t flags = spin_lock_irqsave(&s_##name.lock);                       \
    tidx_ensure_init(&s_##name);                                              \
    if (!s_##name.slots) {                                                    \
        spin_unlock_irqrestore(&s_##name.lock, flags);                        \
        return;                                                               \
    }                                                                         \
    if (s_##name.count * 4u >= s_##name.cap * 3u) {                           \
        if (tidx_grow(&s_##name, keyof) < 0) {                                \
            spin_unlock_irqrestore(&s_##name.lock, flags);                    \
            return;                                                           \
        }                                                                     \
    }                                                                         \
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
        if (slot >= s_##name.cap) {                                           \
            spin_unlock_irqrestore(&s_##name.lock, flags);                    \
            return;                                                           \
        }                                                                     \
        t->prev_field = NULL;                                                 \
        t->next_field = NULL;                                                 \
        s_##name.slots[slot] = t;                                             \
        s_##name.count++;                                                     \
    }                                                                         \
    spin_unlock_irqrestore(&s_##name.lock, flags);                            \
}                                                                             \
                                                                              \
static void name##_remove(task_t* t) {                                        \
    uint64_t flags = spin_lock_irqsave(&s_##name.lock);                       \
    if (!s_##name.slots) {                                                    \
        spin_unlock_irqrestore(&s_##name.lock, flags);                        \
        return;                                                               \
    }                                                                         \
    uint32_t key = keyof(t);                                                  \
    uint32_t idx = tidx_find(&s_##name, key, keyof);                          \
    if (idx >= s_##name.cap) {                                                \
        spin_unlock_irqrestore(&s_##name.lock, flags);                        \
        return;                                                               \
    }                                                                         \
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
    spin_unlock_irqrestore(&s_##name.lock, flags);                            \
}                                                                             \
                                                                              \
void name##_walk(uint32_t key, void (*cb)(task_t*, void*), void* data) {      \
    uint64_t flags = spin_lock_irqsave(&s_##name.lock);                       \
    if (!s_##name.slots) {                                                    \
        spin_unlock_irqrestore(&s_##name.lock, flags);                        \
        return;                                                               \
    }                                                                         \
    uint32_t idx = tidx_find(&s_##name, key, keyof);                          \
    if (idx >= s_##name.cap) {                                                \
        spin_unlock_irqrestore(&s_##name.lock, flags);                        \
        return;                                                               \
    }                                                                         \
    task_t* t = s_##name.slots[idx];                                          \
    while (t) {                                                               \
        task_t* nx = t->next_field;                                           \
        cb(t, data);                                                          \
        t = nx;                                                               \
    }                                                                         \
    spin_unlock_irqrestore(&s_##name.lock, flags);                            \
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

void task_idx_pgid_walk(uint32_t pgid, void (*cb)(task_t*, void*), void* data) {
    pgid_ht_walk(pgid, cb, data);
}
void task_idx_tgid_walk(uint32_t tgid, void (*cb)(task_t*, void*), void* data) {
    tgid_ht_walk(tgid, cb, data);
}

// ── Per-CPU idle task ───────────────────────────────────────────────────
//
// Each CPU owns a private idle task built from the same plumbing any
// other kthread uses.  `do_switch` explicitly context-switches INTO the
// idle task whenever the rq is empty, so the outgoing task's kstack
// becomes idle and safe to reap.  The idle task's entry function is
// `cpu_idle_loop` below: `sti; hlt; sched_yield` forever, bumping RCU
// quiescent state on every lap.
//
// Why per-CPU instead of one shared `s_idle`: context_switch fxsaves
// the outgoing task's FPU state into its ctx buffer.  Two CPUs sharing
// one idle task_t would fxsave into the same buffer concurrently, and
// two CPUs running idle at the same time would step on each other's
// kstack.  Giving every CPU its own idle task_t + kstack kills both.
//
// The mm and files structs are shared because kernel threads don't
// have their own address space or fd table — the global instances
// below are treated as read-only by every CPU's idle task.
static task_mm_t    s_idle_mm    = { .pml4_phys = 0, .mm = NULL, .refs = 1 };
static task_files_t s_idle_files = { .ft = NULL, .lock = SPINLOCK_INIT, .refs = 1 };

extern void proc_trampoline(void);  // kernel/arch/x86_64/process_ctx_switch.asm

// ── Work-stealing: pull-on-idle from random remote CPUs ─────────────────
//
// Called from the idle loop when this CPU has nothing to run.  Picks up
// to STEAL_ATTEMPTS random victims, skipping any whose nr_running is
// below STEAL_MIN_TASKS (never take a CPU's only task).  For each
// chosen victim, tries to steal up to STEAL_BATCH tasks from that
// CPU's highest-priority non-empty level, preserving MLFQ priority
// across the migration.
//
// Random victim selection is O(1) — scanning all CPUs for "busiest"
// would itself become the bottleneck on a 64-core system (Go's
// runtime, Tokio, and musl-sched-lib all converged on random for this
// reason).  With K=4 retries and half-batch steal, expected steal cost
// is 1-2 CAS per idle wake-up regardless of system size.
#define STEAL_ATTEMPTS   4u
#define STEAL_BATCH      16u   // upper bound on tasks moved per victim
#define STEAL_MIN_TASKS  2u    // never steal a CPU's sole task

// Simple per-CPU xorshift64 — seeded from TSC+cpu_id on first use.
// Stored in cpu_t::steal_rng (added below).  No cross-CPU contention.
static inline uint32_t steal_rng_next(cpu_t* c) {
    uint64_t x = c->steal_rng;
    if (UNLIKELY(x == 0)) {
        extern uint64_t tsc_read_ns(void);
        x = tsc_read_ns() ^ ((uint64_t)c->id * 0x9E3779B97F4A7C15ULL);
        if (x == 0) x = 1;
    }
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    c->steal_rng = x;
    return (uint32_t)x;
}

uint8_t sched_try_steal(cpu_t* self);   // forward decl for idle loop
uint8_t sched_try_steal(cpu_t* self) {
    unsigned n = num_cpus();
    if (n < 2) return 0;

    uint8_t took_any = 0;
    for (uint32_t a = 0; a < STEAL_ATTEMPTS; a++) {
        uint32_t r = steal_rng_next(self) % n;
        if (&g_cpus[r] == self) continue;   // skip ourselves
        cpu_t* victim = &g_cpus[r];
        uint32_t vr = __atomic_load_n(&victim->rq.nr_running,
                                        __ATOMIC_RELAXED);
        if (vr < STEAL_MIN_TASKS) continue;

        // Pick the victim's highest-priority non-empty level so we
        // preserve priority across migration.
        for (uint8_t lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            chaselev_deque_t* vdq = &victim->rq.levels[lvl];
            if (chaselev_size_approx(vdq) == 0) continue;

            // Steal up to half (capped at STEAL_BATCH) from this level.
            uint32_t take = chaselev_size_approx(vdq) / 2;
            if (take == 0) take = 1;
            if (take > STEAL_BATCH) take = STEAL_BATCH;

            for (uint32_t i = 0; i < take; i++) {
                void* v = chaselev_steal(vdq);
                if (v == CHASELEV_ABORT) {        // CAS lost
                    cpu_relax();
                    continue;
                }
                if (!v) break;                     // empty
                task_t* t = (task_t*)v;
                __atomic_fetch_sub(&victim->rq.nr_running, 1,
                                     __ATOMIC_RELAXED);

                // Push onto our own deque at the same MLFQ level.
                // We're the owner here — chaselev_push is lock-free
                // from our side and doesn't contend with thieves on
                // OUR queue (there shouldn't be any at this moment
                // since we just woke from idle).
                if (!chaselev_push(&self->rq.levels[lvl], t)) {
                    // Our own queue full — give the task back.
                    // Pushing to victim again isn't easy from this
                    // side (we're a thief w.r.t them), so enqueue
                    // locally once space clears.  For now, re-try by
                    // popping oldest and re-pushing victim — but at
                    // 512 slots this is pathological.  Panic for now.
                    extern void kprintf(const char*, ...);
                    kprintf("[sched] PANIC: steal-side rq overflow\n");
                    for (;;) __asm__ volatile("cli; hlt");
                }
                __atomic_fetch_add(&self->rq.nr_running, 1,
                                     __ATOMIC_RELAXED);
                took_any = 1;
            }
            if (took_any) break;      // stop at first productive level
        }
        if (took_any) return 1;       // one productive victim is enough
    }
    return took_any;
}

// Idle loop — runs on each CPU's private idle task_t + kstack when the
// scheduler has nothing else to give it.  Spins between sti/hlt and
// sched_yield so:
//   - hlt parks the CPU between interrupts (power + thermal)
//   - the sched_yield after each hlt rechecks the rq (if an IPI or
//     timer enqueued work while we were halted, sched_yield's do_switch
//     will dispatch it)
//   - rcu_note_qs advances this CPU's RCU counter every lap, which is
//     the ONLY way synchronize_rcu() from another CPU can ever complete
//     while we're idle
//
// preempt_depth stays 0 the entire time; timer IRQs freely preempt us.
//
// Work-stealing (phase 3): before halting, try to steal from a few
// random remote CPUs.  If successful, the stolen task is enqueued
// locally and sched_yield below picks it up naturally.  If no steal
// succeeds, we fall through to sti;hlt and wait for an IPI/IRQ.
static void cpu_idle_loop(void) {
    for (;;) {
        rcu_note_qs();

        // Only attempt to steal if our own rq is empty — otherwise we
        // have work and sched_yield below will find it.
        cpu_t* c = this_cpu();
        if (__atomic_load_n(&c->rq.nr_running, __ATOMIC_RELAXED) == 0) {
            // Returns true if something was enqueued locally.
            extern uint8_t sched_try_steal(cpu_t* self);
            (void)sched_try_steal(c);
        }

        __asm__ volatile("sti\nhlt");
        sched_yield();
    }
}

// Public entry: cpu_init_ap calls this after its final setup to hand
// the AP off to the scheduler forever.  We do NOT go through sched_yield
// because the AP's current == idle, and sched_yield → do_switch → the
// identity context_switch-into-self would just return straight back
// here, landing in the unreachable fallback below cpu_init_ap.  By
// calling cpu_idle_loop directly we guarantee the AP starts executing
// the real idle body on its own kstack.
void sched_enter_idle(void) {
    cpu_idle_loop();
    for (;;) __asm__ volatile("cli; hlt");  // unreachable
}

void sched_init_idle_for_cpu(uint32_t id) {
    // Lazy-init of the shared idle mm's pml4: must point at the kernel
    // PML4 so context_switch INTO idle reloads cr3 off of whatever
    // user mm was previously installed on this CPU.  Leaving pml4_phys
    // at 0 would make context_switch skip the cr3 reload, which
    // leaves a DEAD user PML4 in cr3 after the departing task gets
    // its PML4 freed by task_free_rcu — any later kernel memory
    // access on that CPU then page-faults into garbage.
    //
    // Initialisation is idempotent; repeat calls just rewrite the same
    // value.  vmm_init must have run before we get here (kmain ordering
    // guarantees this).
    if (!s_idle_mm.pml4_phys)
        s_idle_mm.pml4_phys = vmm_kernel_pml4_get();

    task_t* idle = (task_t*)kmalloc(sizeof(task_t));
    if (!idle) {
        serial_puts_dbg("[sched] PANIC: idle task alloc failed for cpu=");
        serial_hex_dbg((uint64_t)id);
        for (;;) __asm__ volatile("cli; hlt");
    }
    __builtin_memset(idle, 0, sizeof(*idle));
    idle->pid          = 0;
    idle->tgid         = 0;
    idle->ppid         = 0;
    idle->flags        = TASK_FLAG_KTHREAD;
    idle->state        = TASK_RUNNING;
    idle->mm_shared    = &s_idle_mm;
    idle->files_shared = &s_idle_files;
    idle->home_cpu     = id;
    idle->last_ran_cpu = id;
    __builtin_memcpy(idle->comm, "idle", 5);

    // Capture a valid FPU state for the first fxrstor into this task.
    __asm__ volatile("fxsave %0" : "=m"(idle->ctx.fxsave_buf));

    // Build the initial kstack frame so context_switch INTO this task
    // lands in cpu_idle_loop via proc_trampoline.  Mirrors
    // task_create_kthread's layout: the stack we hand to context_switch
    // has, from high to low:
    //     [ 0 ]          ← dummy "return RIP" for proc_trampoline itself
    //     proc_trampoline
    //     rbx rbp
    //     r12 = entry     ← proc_trampoline reads this into rax / call
    //     r13 r14 r15
    // and context_switch's pops happen in reverse: r15..rbx, then ret
    // pops proc_trampoline, which `sti; call r12` enters cpu_idle_loop.
    virt_addr_t kstack_top = kstack_alloc();
    idle->kstack_top = kstack_top;
    idle->fs_base    = 0;
    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;                          // dummy retaddr (unused)
    *(--stk) = (uint64_t)proc_trampoline;  // ret from context_switch lands here
    *(--stk) = 0;                          // rbx
    *(--stk) = 0;                          // rbp
    *(--stk) = (uint64_t)cpu_idle_loop;    // r12 — entry function
    *(--stk) = 0;                          // r13
    *(--stk) = 0;                          // r14
    *(--stk) = 0;                          // r15
    idle->ctx.rsp = (uint64_t)stk;

    cpu_t* c = &g_cpus[id];
    c->idle = idle;

    // Leave c->current == idle ONLY on the BSP's very first call from
    // sched_init — the BSP is about to keep running init_kthread in
    // its existing task, not idle, so the scheduler will overwrite
    // c->current via the next sched_add/context_switch.  On APs,
    // cpu_init_ap immediately calls sched_yield which context-switches
    // AWAY from c->current; if c->current is idle (with a valid ctx),
    // the switch correctly saves idle's state and runs the next task.
    if (!c->current) c->current = idle;
}

static volatile uint32_t s_tick_count  = 0;

// ── Internal helpers ──────────────────────────────────────────────────────
//
// Every helper below takes a cpu_rq_t* (pointing at the owning CPU's rq)
// so the same code works for any CPU.  Callers must hold that CPU's
// rq_lock.

// Enqueue at the task's current mlfq_level.  Still called under
// rq_lock for the sleep_head/zombie_head fields, but the runqueue
// itself no longer needs it — chaselev_push is lock-free.
static void enqueue_on(cpu_rq_t* rq, task_t* p) {
    p->state = TASK_READY;
    p->next  = NULL;
    if (p->mlfq_ticks_left == 0)
        p->mlfq_ticks_left = s_quanta[p->mlfq_level];

    uint8_t lvl = p->mlfq_level;
    if (UNLIKELY(!chaselev_push(&rq->levels[lvl], p))) {
        // 512 slots per level per CPU exhausted.  At Linux scale this
        // means the caller is leaking runnable tasks; panic with
        // enough context rather than silently drop.
        extern void kprintf(const char*, ...);
        kprintf("[sched] PANIC: rq level %u overflow on cpu %u\n",
                (uint32_t)lvl, (uint32_t)((rq - &g_cpus[0].rq) ? 0 : 0));
        for (;;) __asm__ volatile("cli; hlt");
    }
    __atomic_fetch_add(&rq->nr_running, 1, __ATOMIC_RELAXED);
}

// Dequeue from the highest-priority non-empty level.  Owner-only —
// chaselev_pop is the LIFO side; preserves the "run recent tasks first"
// semantics of the old linked-list version.
static task_t* dequeue_local(cpu_rq_t* rq) {
    for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
        task_t* p = (task_t*)chaselev_pop(&rq->levels[i]);
        if (p) {
            __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);
            return p;
        }
    }
    return NULL;
}

// Pick the target CPU for a new task.  Round-robin across all online
// CPUs via a single relaxed atomic counter — O(1), cache-friendly,
// no global lock.  Strictly correct under SMP: even if two CPUs
// contend on s_placement_cursor at the same time they just end up
// with the same modulo result on rare collisions, which is fine
// (the load balancer's job in later phases is to fix imbalance, not
// this placement hint's).
//
// Phase 9-7 replaces this with least-loaded-or-cache-affine.  For
// Phase 9-5 the goal is simply "new tasks actually land on APs", and
// round-robin achieves that with zero measurable overhead.
// Phase 9-5 keeps ALL tasks pinned to the BSP (CPU 0).  APs are brought
// up, run the idle loop, handle IPIs (TLB shootdown, RCU quiescence,
// reschedule), and advance their rcu_qs_count via sched_tick — but they
// do NOT run user-visible tasks yet.
//
// Phase 9-6g: round-robin placement across every online CPU.
//
// Every cross-CPU sleep/wake path that matters for a real userland
// has been audited and fixed:
//
//   9-6a  sys_wait children walk → lock-free Treiber stack
//   9-6b  pty/tty wait queues    → canonical 1/2/3/4 pattern
//   9-6c  epoll has_ready flag   → explicit atomic ordering
//   9-6d  pipe read/write        → signal-interruptible + commented
//   9-6e  unix socket (all 6)    → signal-interruptible + commented
//   9-6f  AHCI submit path       → lock-free MPSC, stack reqs
//
// Phase 9-5 already fixed the scheduler primitive (wake_pending),
// the IPI handlers, the per-CPU idle task, the fb spinlock, and
// rcu_note_qs on the preempted/empty-rq path.
//
// s_placement_cursor is a plain uint32_t incremented with a
// __atomic_fetch_add; the modulo by g_num_cpus is done outside the
// atomic so the cursor wraps naturally across 2^32 sched_add calls
// (= weeks at sustained fork/s-scale).  Kernel threads also round-
// robin: every kthread path we have (ahci_io_thread, virtio_net,
// keyboard/mouse, irq_wait waiters) is CPU-agnostic.
static volatile uint32_t s_placement_cursor = 0;
static inline uint32_t pick_home_cpu(void) {
    uint32_t n = g_num_cpus ? g_num_cpus : 1;
    uint32_t i = __atomic_fetch_add(&s_placement_cursor, 1, __ATOMIC_RELAXED);
    return i % n;
}

// Shortcut to the owning CPU's rq.
// Where to enqueue / look up this task for wake / wait / sleep list
// ops.  Prefer last_ran_cpu (cache-warm) over home_cpu (static).  If
// last_ran_cpu is stale / OOB, fall back to home_cpu.  Relaxed load is
// fine — even a torn read just means we enqueue on a cold CPU, which
// work stealing will re-balance on its next idle cycle.
static inline cpu_t* cpu_of_task(task_t* t) {
    uint32_t lr = __atomic_load_n(&t->last_ran_cpu, __ATOMIC_RELAXED);
    if (lr < MAX_CPUS) return &g_cpus[lr];
    return &g_cpus[t->home_cpu];
}

// ── Public API ────────────────────────────────────────────────────────────

void sched_init(void) {
    // Per-CPU run queues are BSS-zeroed; rq_lock was initialised by
    // cpu_init_bsp.  Build the BSP's idle task (APs get theirs in
    // cpu_init_ap via the same helper).  After this call,
    // this_cpu()->current / ->idle both point at a fresh per-CPU
    // task_t with its own fxsave buffer, so concurrent context
    // switches on different CPUs don't stomp each other's state.
    sched_init_idle_for_cpu(cpu_id());
    timer_register_tick(sched_tick);
}

// Nudge a CPU that we just enqueued work onto.  The target must be a
// DIFFERENT CPU than the current one — for same-CPU wakes, callers
// just set reschedule_pending locally.
//
// Two-step: set reschedule_pending on the target (so if the IPI is
// queued-but-not-yet-delivered, the target's next tick picks up the
// flag anyway), then send VEC_IPI_RESCHEDULE which forces an IRQ on
// the target even if it's sitting in `sti; hlt`.
//
// No memory barrier on the flag write: x2APIC ICR wrmsr is a serialising
// instruction, so all prior stores are visible to the receiver before
// it enters the IPI handler.
static inline void kick_remote_cpu(cpu_t* target) {
    target->reschedule_pending = 1;
    lapic_send_ipi(target->apic_id, VEC_IPI_RESCHEDULE);
}

void sched_add(task_t* proc) {
    // Register the first user process as the init/orphan-reaper task.
    if (!g_init_task && !(proc->flags & TASK_FLAG_KTHREAD))
        g_init_task = proc;

    // Assign a home CPU once; the task stays there until Phase 9-7
    // adds work stealing.  Before SMP bring-up (g_num_cpus == 1) this
    // is always 0 — everything early in boot stays on the BSP, which
    // is what we want so init / svcmgr / login spawn without ever
    // waiting on an AP that hasn't come online yet.
    proc->home_cpu     = pick_home_cpu();
    proc->last_ran_cpu = proc->home_cpu;    // initial affinity

    pid_ht_insert(proc);
    task_idx_insert(proc);

    cpu_t* target = &g_cpus[proc->home_cpu];
    uint64_t flags = spin_lock_irqsave(&target->rq_lock);
    enqueue_on(&target->rq, proc);
    spin_unlock_irqrestore(&target->rq_lock, flags);

    // If the new task went to a different CPU than the one we're
    // running on, wake it immediately — otherwise it could sit in
    // hlt until its next timer tick (up to 1 ms at SCHED_HZ=1000).
    if (target != this_cpu())
        kick_remote_cpu(target);
}

// ── Per-task child list ───────────────────────────────────────────────────
//
// The children list is a lock-free Treiber stack: head pointer is
// parent->children, intra-list link is task_t.child_next.
//
// Writers:
//   - fork / spawn:  parent calls task_child_add(self, new_child).
//                    "self" is the running CPU, so from the parent's
//                    POV this is a self-write — but ANOTHER task
//                    exiting concurrently and reparenting into the
//                    same parent (only happens if parent == g_init_task)
//                    is a cross-CPU writer.  CAS-prepend serialises.
//   - exit reparent: the exiting task walks its OWN children (no
//                    contention — only the exiter touches that list
//                    during the walk) and splices the whole chain
//                    onto g_init_task->children via one CAS.  Batch
//                    splice turns N reparents into O(1) atomics.
//
// Reader (also reaper):
//   - sys_wait:      calls task_children_reap(self, pid, ...) which
//                    atomically XCHG's the whole children chain to
//                    NULL, walks it privately looking for a matching
//                    zombie, CAS-splices the survivors back on top of
//                    anything a concurrent writer added in the
//                    meantime.  Uses one XCHG + one CAS per wait
//                    iteration, independent of children count.
//
// Why lock-free, not a spinlock:
//   Uncontested cost is the same (~20 cycles for one atomic op), but
//   under mass-exit reparenting-to-init the writers scale linearly
//   instead of serialising on a single lock.  And sys_exit's batch
//   splice is O(1) atomics no matter how many orphans it has.  No
//   new spinlock means no new LOCKS.md entry and no new acquisition
//   order to reason about.
//
// x86-64 ordering notes (relied on throughout):
//   - CAS-release pairs with xchg-acquire: any store made by a
//     writer before its successful CAS is visible to a reader after
//     its xchg drains the list.
//   - child->child_next writes inside task_child_add_chain happen
//     BEFORE the final CAS that publishes the chain head, so readers
//     see fully-initialised links.
//   - child->state = TASK_ZOMBIE in sys_exit is sequenced before
//     sched_add_zombie's rq_lock operations and before
//     signal_send → sched_wake → parent rq_lock, which in turn
//     pairs with sched_sleep's rq_lock acquire.  So a parent that
//     re-walks its children after a wake is guaranteed to see any
//     child that became a zombie on another CPU.

// Prepend a single child to parent's children list with one CAS.
void task_child_add(task_t* parent, task_t* child) {
    task_t* old_head = __atomic_load_n(&parent->children, __ATOMIC_RELAXED);
    do {
        child->child_next = old_head;
    } while (!__atomic_compare_exchange_n(&parent->children, &old_head, child,
                                            /*weak=*/0,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
}

// Prepend an entire chain (head…tail already linked via child_next)
// onto parent's children list with one CAS.  Used by sys_exit's
// reparenting loop to move every orphan to g_init_task in one op.
// Caller supplies both endpoints; intermediate next pointers are
// already set up.  No-op if head is NULL.
void task_child_add_chain(task_t* parent, task_t* head, task_t* tail) {
    if (!head || !tail) return;
    task_t* old_head = __atomic_load_n(&parent->children, __ATOMIC_RELAXED);
    do {
        tail->child_next = old_head;
    } while (!__atomic_compare_exchange_n(&parent->children, &old_head, head,
                                            /*weak=*/0,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
}

// Atomically drain parent's children list, walk it looking for a
// reapable zombie, and splice the survivors back.
//
//   parent      — who we're waiting on behalf of (= g_current normally).
//   target_pid  — 0 for "any child", else require child->pid == target_pid.
//   out_found   — if non-null, set to 1 iff at least one child matched
//                 target_pid (regardless of zombie state).  Used by the
//                 caller to distinguish "no match → ECHILD" from "match
//                 found but still running → sleep".
//
// Returns the reaped zombie (unlinked from the children list; caller
// is responsible for sched_reap_zombie + process_destroy) or NULL.
//
// Walker guarantee: any writer that published a child via
// task_child_add{,_chain} BEFORE this function's XCHG is visible in
// the private chain.  Writers that race with the XCHG land on the
// fresh empty head; the next call will pick them up.
task_t* task_children_reap(task_t* parent, uint32_t target_pid,
                            uint8_t* out_found) {
    if (out_found) *out_found = 0;
    if (!parent) return NULL;

    task_t* chain = __atomic_exchange_n(&parent->children, (task_t*)NULL,
                                          __ATOMIC_ACQ_REL);
    if (!chain) return NULL;

    task_t*  zombie     = NULL;
    task_t*  keep_head  = NULL;
    task_t*  keep_tail  = NULL;
    uint8_t  found      = 0;

    task_t* c = chain;
    while (c) {
        task_t* next = c->child_next;
        c->child_next = NULL;

        uint8_t matches = (target_pid == 0 || c->pid == target_pid);
        if (matches) found = 1;

        if (!zombie && matches && c->state == TASK_ZOMBIE) {
            zombie = c;
        } else {
            // Keep on the survivor chain.  Push-to-head preserves
            // nothing about original order, which is fine — the
            // children list is unordered.
            c->child_next = keep_head;
            keep_head = c;
            if (!keep_tail) keep_tail = c;
        }
        c = next;
    }

    if (out_found) *out_found = found;

    if (keep_head)
        task_child_add_chain(parent, keep_head, keep_tail);

    return zombie;
}

// Move every child of `from` onto `to`'s children list in a single
// splice, updating each child's ppid in-flight.  Used by the exit
// paths (sys_exit + signal.c fatal kill) to reparent orphans to init.
//
// Only the dying task walks its own children list here, so the drain
// is a plain load — no race with itself.  The publish onto `to` is
// the canonical CAS-prepend via task_child_add_chain.
void task_children_reparent(task_t* from, task_t* to) {
    if (!from || !to || from == to) return;

    task_t* chain = from->children;
    from->children = NULL;
    if (!chain) return;

    task_t* head = NULL;
    task_t* tail = NULL;
    uint32_t new_ppid = to->pid;

    task_t* c = chain;
    while (c) {
        task_t* next = c->child_next;
        c->ppid = new_ppid;
        c->child_next = head;
        head = c;
        if (!tail) tail = c;
        c = next;
    }

    task_child_add_chain(to, head, tail);
}

// ── sched_tick ────────────────────────────────────────────────────────────
// Called from timer IRQ — only sets flags/counters, never touches the stack.

// sched_tick — runs 1000 times per second per CPU, in timer ISR context.
//
// ~99% of ticks do ZERO shared-state work: no sleeper expires, no boost,
// just tick down the running task's local quantum.  Taking rq_lock on
// every tick is wasteful — it adds ~35 cycles × 1000/sec = 35k cycles/sec
// of pure overhead for nothing.
//
// Optimization: do the cheap no-lock path first (quantum tick + peek at
// sleep_head / boost counter), and only take the lock if we actually
// need to mutate shared state.
//
// What's safe without the lock:
//   - c->current and c->current->mlfq_* fields are strictly per-CPU.
//     Only this CPU's sched code ever touches them.  No lock needed.
//   - c->reschedule_pending is a flag this CPU sets and this CPU reads
//     on the next preempt check.  No cross-CPU write.
//   - Reading rq->sleep_head as a pointer is racy under SMP (another
//     CPU might be adding to it via sched_wake), but a stale-but-
//     non-NULL read just means we take the lock and re-check; a
//     stale-NULL read just means we miss a wakeup by one tick, which
//     is fine because the next tick catches it.
//
// What needs the lock:
//   - Walking and mutating sleep_head.
//   - Mutating runqueue heads/tails during priority boost.
void sched_tick(void) {
    s_tick_count++;

    // Fallback for lost MSI-X: rescan AHCI completions every tick.
    // If an IRQ was swallowed, this catches it within ~1ms.
    ahci_poll_completions();

    cpu_t*    c  = this_cpu();
    cpu_rq_t* rq = &c->rq;

    // ── Heartbeat — "this CPU is still ticking" ───────────────────────
    // Print once per ~second per CPU so serial.txt shows whether a
    // freeze is a real lost-wakeup (all CPUs parked in idle, heartbeats
    // still flowing) or a kernel wedge (some CPU's heartbeat stops).
    // Uses the locked serial_puts_dbg so SMP-safe.  Debug aid — flip
    // the #define to 0 to silence.
#define SCHED_TICK_HEARTBEAT 0
#if SCHED_TICK_HEARTBEAT
    c->sched_ticks++;
    if ((c->sched_ticks & 0x3FF) == 0) {  // every 1024 ticks ≈ 1 sec at 1kHz
        serial_puts_dbg("[hb] cpu");
        serial_hex_dbg((uint64_t)c->id);
    }
#endif

    // ── Per-CPU timerfd drain ───────────────────────────────────────────
    // Atomic fast-path in timerfd_tick — returns O(1) if no expired
    // timers on this CPU.  Drain path only runs when needed.
    extern void timerfd_tick(void);
    timerfd_tick();

    // ── Local-only work (no lock) ───────────────────────────────────────
    // Tick down the running task's quantum.
    if (c->current && c->current != c->idle) {
        if (c->current->mlfq_ticks_left > 0)
            c->current->mlfq_ticks_left--;
        if (c->current->mlfq_ticks_left == 0)
            c->reschedule_pending = 1;
    } else {
        // Idle is running.  Setting reschedule_pending on every tick is
        // how do_switch / pick_next discovers tasks that were enqueued
        // via sched_yield (or any other path that doesn't go through
        // sched_wake).  Without this, idle would only ever yield to a
        // freshly woken task — yield-then-runnable patterns would hang
        // until something else fired sched_wake.
        c->reschedule_pending = 1;
    }

    // Check whether we need to touch shared state at all.  Reading
    // sleep_head without the lock is a data race under SMP in the
    // strict sense, but any racy read is safe here:
    //   - Non-NULL: we'll take the lock and re-check inside; worst
    //     case one extra lock acquisition.
    //   - NULL: a concurrent sched_wake might be adding a sleeper
    //     right now, but missing its expiry by one tick is harmless
    //     — the next tick catches it.
    int need_lock = 0;
    if (rq->sleep_head) need_lock = 1;
    if (s_tick_count % BOOST_INTERVAL == 0) need_lock = 1;

    if (!need_lock) return;

    // ── Shared-state work (under lock) ──────────────────────────────────
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

    // Priority boost: move every task in levels 1-N back to level 0.
    // With Chase-Lev deques we drain each lower level by popping (which
    // may race with thieves on other CPUs — harmless, the thief gets
    // the task before we boost it, but then it'll re-enter at its
    // level on the thief's CPU and be boosted on a later tick there).
    if (s_tick_count % BOOST_INTERVAL == 0) {
        for (uint8_t i = 1; i < MLFQ_LEVELS; i++) {
            for (;;) {
                task_t* t = (task_t*)chaselev_pop(&rq->levels[i]);
                if (!t) break;
                t->mlfq_level      = 0;
                t->mlfq_ticks_left = s_quanta[0];
                // Re-push into level 0.  We decremented nr_running in
                // the pop, the push below bumps it again — net zero.
                if (UNLIKELY(!chaselev_push(&rq->levels[0], t))) {
                    extern void kprintf(const char*, ...);
                    kprintf("[sched] PANIC: boost overflow\n");
                    for (;;) __asm__ volatile("cli; hlt");
                }
                __atomic_fetch_add(&rq->nr_running, 1, __ATOMIC_RELAXED);
                __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);
            }
        }
        // Boost the currently running task too (local state, but done
        // here so the boost semantics are visible in one place).
        if (c->current && c->current != c->idle) {
            c->current->mlfq_level      = 0;
            c->current->mlfq_ticks_left = s_quanta[0];
        }
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

    // ── Deferred preemption for mid-exit tasks ──────────────────────────
    //
    // A preempted (timer-triggered) switch MUST NOT yank a task off its
    // own kstack after sys_exit / signal.c has transitioned it to
    // TASK_ZOMBIE or TASK_DEAD but before it has finished the exit
    // unwinding (signal_send(parent, SIGCHLD), sched_yield).  If we let
    // do_switch(1) context-switch away from such a task, its saved rsp
    // points inside sys_exit, and because the task's state is already
    // ZOMBIE it is never re-entered via any runqueue — the parent waits
    // forever for a SIGCHLD that never gets sent, and the zombie never
    // reaches the final hlt loop.  Observed in the wild during
    // interactive bash under pty: a command's exit hangs when a timer
    // tick arrives between `state = TASK_ZOMBIE` and `signal_send`.
    //
    // The fix: if preempted and cur is mid-exit, bail out of do_switch
    // WITHOUT switching.  The exiting task will reach its own voluntary
    // sched_yield (do_switch(0), preempted=0) within a handful of
    // instructions and that path handles the zombie correctly (next =
    // idle, cur not requeued, kstack left on zombie_head until reap).
    // The deferred preempt is fine — reschedule_pending stays set, and
    // the next tick picks it up after the voluntary switch.
    task_t* cur_check = c->current;
    if (preempted && cur_check != c->idle &&
        (cur_check->state == TASK_ZOMBIE || cur_check->state == TASK_DEAD)) {
        // Bump rcu qs — the task is at a non-RCU-reader point (it's
        // running its own sys_exit tail), so the current CPU is
        // quiescent even though we don't context-switch.
        rcu_note_qs();
        return;
    }

    // Pick the next runnable task under the local rq_lock.  If nothing
    // is ready and this is a voluntary yield/sleep, halt until an IRQ
    // wakes something — releasing the lock across the hlt so wakers
    // on other CPUs (or our own IRQ path) can actually enqueue work.
    uint64_t flags = spin_lock_irqsave(&c->rq_lock);
    next = dequeue_local(rq);
    if (!next) {
        // Nothing runnable.  If the current task is a ZOMBIE, SLEEPING,
        // or DEAD we CANNOT just `let it keep running` — its kstack may
        // be freed out from under us at any moment by a reaper on another
        // CPU, and scheduled-out sleepers shouldn't hog their kstack in
        // a spin anyway.  Switch to the per-CPU idle task instead — its
        // kstack is private and lives forever, so the subsequent hlt
        // loop runs on a stable foundation.
        //
        // If the current task is merely RUNNING and we're on the
        // preempted/early path (timer) just bail — that task keeps its
        // slice.  We still note a QS: sched_preempt only reaches this
        // call with preempt_depth==0, so the interrupted context is
        // not inside an RCU reader.
        task_t* cur = c->current;
        uint8_t needs_park = (cur != c->idle) &&
                             (cur->state != TASK_RUNNING);
        if (preempted && !needs_park) {
            spin_unlock_irqrestore(&c->rq_lock, flags);
            rcu_note_qs();
            return;
        }
        // Switch to idle so the current task's kstack is freed from the
        // CPU.  For needs_park (zombie / sleeping / dead), this is the
        // ONLY correct path — returning to cur would resume execution
        // on a kstack that task_free_rcu / sched_wake bookkeeping might
        // tear out from under us.  For the voluntary-yield path with a
        // runnable cur, switching to idle is also fine: idle just hlts
        // until something gets enqueued, then we come back around.
        next = c->idle;
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
    // Cache-affinity hint: remember where this task last ran so
    // sched_wake can route future wakes to the same CPU.  Plain
    // store — only the owning CPU reads this value with a relaxed
    // load, races are benign (stale ≤ one context switch window).
    next->last_ran_cpu = c->id;
    c->context_switches++;

    // Release the rq_lock BEFORE context_switch — holding it across
    // the switch deadlocks: the new task might try to take the same
    // lock before the old task's unlock ever runs.  But we keep IRQs
    // DISABLED across the switch.  Re-enabling here opens a window
    // where prev's kstack is still in use (RSP still on it) but
    // c->current already points at next.  An IRQ in that window whose
    // handler wakes prev elsewhere creates two-CPUs-on-one-kstack
    // concurrency (prev's ctx.rsp is stale but valid, and a remote CPU
    // can pick prev off its rq and resume on the same kstack we're
    // still using).  context_switch's ret restores IF from RFLAGS in
    // the incoming task's saved frame, so the incoming task wakes with
    // the correct IF state — we only suppress IRQs across the switch
    // itself.
    spin_unlock(&c->rq_lock);

    // Every context switch is an RCU quiescent state for this CPU: the
    // outgoing task can no longer be in a reader section, and the
    // incoming task starts fresh.  Bumping the counter here lets
    // synchronize_rcu() observe that this CPU has made progress.
    rcu_note_qs();

    tss_set_rsp0(next->kstack_top);

    // Userspace TLS (%fs) — per task, not per CPU.  Cheap relative to
    // the CR3 write below; unconditionally restoring avoids tracking
    // a per-CPU shadow of the MSR.
    {
        uint64_t fsb = next->fs_base;
        // Non-canonical wrmsr is a #GP *in the scheduler* — and tasks
        // created outside the user paths (idle, kthreads) come from
        // kmalloc with whatever was in the slab.  Anything outside the
        // user half simply isn't a TLS pointer: write 0.
        if (fsb >> 47) fsb = 0;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100u /*MSR_FS_BASE*/),
                         "a"((uint32_t)fsb), "d"((uint32_t)(fsb >> 32)));
    }

    // ── Phase 9-7 TLB shootdown bookkeeping ──────────────────────────────
    // Update the per-mm cpumasks *before* touching CR3.  The mask is
    // "CPUs whose CR3 currently points at this mm's pml4" — a pure
    // accounting shadow of CR3.  Only user mms participate (kthreads
    // run in whatever pml4 was previously loaded → lazy TLB; they hold
    // no user mappings themselves, so any pending shootdown for a user
    // mm need not target them).
    //
    // Clear prev's bit first: after context_switch writes CR3 for a
    // different mm, this CPU's TLB for prev's user entries is gone
    // (we don't use global pages, so mov-to-cr3 is a full flush).
    // If prev's mm equals next's mm (two tasks sharing one address
    // space, e.g. threads), context_switch will skip the CR3 write,
    // and we set the same bit right back below — net no-op.
    uint32_t cpu_id = c->id;
    if (prev->mm_shared && prev->mm_shared->mm)
        task_mm_cpumask_clear(prev->mm_shared, cpu_id);
    if (next->mm_shared && next->mm_shared->mm)
        task_mm_cpumask_set(next->mm_shared, cpu_id);

    context_switch(&prev->ctx, &next->ctx, next->mm_shared->pml4_phys);

    signal_deliver_pending(0);

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
        task_t* self = g_current;
        cpu_t* home  = cpu_of_task(self);
        uint64_t flags = spin_lock_irqsave(&home->rq_lock);

        // ── Lost-wakeup protection (SMP) ───────────────────────────────
        // If a waker fired sched_wake on us BETWEEN the caller's cond
        // check and here, the waker observed state != TASK_SLEEPING and
        // recorded the attempt in `wake_pending` instead of dropping it.
        // Consume the flag and return WITHOUT sleeping — the caller's
        // outer loop will re-check the condition and see the updated
        // state.  Under UP wake_pending is always 0 at this point
        // because there's no other CPU to race with.
        if (self->wake_pending) {
            self->wake_pending = 0;
            spin_unlock_irqrestore(&home->rq_lock, flags);
            c->reschedule_pending = 0;
            return;
        }

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
    cpu_t* home = cpu_of_task(proc);
    uint64_t flags = spin_lock_irqsave(&home->rq_lock);

    // ── Lost-wakeup protection (SMP) ───────────────────────────────────
    // The classic race:
    //   CPU A (sleeper):                CPU B (waker):
    //     check cond → 0
    //                                     set cond = 1
    //                                     sched_wake(sleeper)
    //                                       sees state != SLEEPING → DROPS
    //     sched_sleep → state=SLEEPING
    //     ...never woken
    //
    // Fix: instead of dropping the wake, STASH it in wake_pending.  The
    // sleeper's sched_sleep checks the flag under the same rq_lock after
    // setting its own state to SLEEPING, and bails out if set.  This
    // makes every wake eventually take effect, regardless of the exact
    // interleaving between waker and sleeper.
    if (proc->state != TASK_SLEEPING) {
        proc->wake_pending = 1;
        spin_unlock_irqrestore(&home->rq_lock, flags);
        return;
    }
    // Remove from the target CPU's sleep list.
    task_t** pp = &home->rq.sleep_head;
    while (*pp && *pp != proc) pp = &(*pp)->next;
    if (*pp) *pp = proc->next;
    proc->next = NULL;

    // Power-of-two-choices load balancing at wake time: if home is
    // loaded, pick one random alternate CPU and compare.  If the alt
    // is clearly less loaded, migrate the wake there.  This keeps
    // tasks flowing to idle CPUs proactively (sched_wake) rather than
    // only reactively (steal-on-idle), which matters for workloads
    // where one CPU is persistently busier (BSP runs init/svcmgr/net/
    // pcache etc) and its sleepers would otherwise accumulate.
    //
    // Only O(1) per wake — two nr_running loads, one random index.
    // Migration requires switching rq_locks so we keep home's lock
    // held only across the sleep-list unlink, then either enqueue
    // locally or drop the lock and re-acquire target's.
    cpu_t* target = home;
    unsigned n = num_cpus();
    if (n >= 2) {
        uint32_t home_load = __atomic_load_n(&home->rq.nr_running,
                                               __ATOMIC_RELAXED);
        // Two migration triggers:
        //   (a) home is loaded AND the random alt is STRICTLY less
        //       loaded — classic power-of-2-choices balancing
        //   (b) the random alt is completely idle (nr_running==0)
        //       even if home has just this one task — because a
        //       persistently-busy CPU with load==1 can still be 5x
        //       slower than an idle CPU on another core (BSP vs AP,
        //       shared MSI-X target, ISR-heavy CPU, etc).  An idle
        //       CPU is always a win: the task runs immediately there.
        uint32_t r = steal_rng_next(this_cpu()) % n;
        cpu_t* alt = &g_cpus[r];
        if (alt != home) {
            uint32_t alt_load = __atomic_load_n(&alt->rq.nr_running,
                                                 __ATOMIC_RELAXED);
            if (alt_load == 0 ||
                (home_load >= 2 && alt_load + 1 < home_load))
                target = alt;
        }
    }

    if (target != home) {
        // Migrate.  Race hazard: between releasing home's lock and
        // taking target's, a second sched_wake for the same proc from
        // another CPU would see last_ran_cpu=target, take target's
        // lock, observe state==TASK_SLEEPING, silently fail to remove
        // proc from target's sleep_head (proc isn't there), and
        // enqueue proc — then we enqueue again on target's rq.
        // Double-enqueue → corrupted Chase-Lev slots → crash.
        //
        // Fix: transition state OUT of SLEEPING *before* releasing
        // home's lock.  A racing sched_wake on the second CPU then
        // sees state != TASK_SLEEPING and takes the wake_pending
        // fast-return path instead of trying to re-enqueue.
        proc->state = TASK_READY;
        __atomic_store_n(&proc->last_ran_cpu, target->id,
                           __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&home->rq_lock, flags);

        flags = spin_lock_irqsave(&target->rq_lock);
        // enqueue_on re-sets state=TASK_READY (idempotent) and also
        // does mlfq_ticks_left housekeeping + push + nr_running bump.
        enqueue_on(&target->rq, proc);
        home = target;  // reuse variable for the rest of the function
    } else {
        enqueue_on(&home->rq, proc);
    }

    // Decide whether to ask the target CPU to reschedule immediately.
    //   - If it's running idle, we MUST request a switch: otherwise the
    //     CPU would sit in hlt until its next timer tick before picking
    //     up the freshly-woken task.  That's the root cause of the
    //     "bash frozen after ps" live-wait: ps's last wake landed on an
    //     idle CPU, and without this we waited a full tick (often much
    //     more under SDL display pacing) for bash to run again.
    //   - Otherwise, only preempt if the woken task strictly outranks
    //     the current one.
    //
    // Under SMP `home->current` may be stale from the waker's POV; the
    // conservative choice on any ambiguity is to raise reschedule_pending
    // and send the IPI — a wasted IPI is ~500 cycles, a missed wake is
    // a stuck task.  We accept the IPI cost as the price of correctness.
    // Always raise reschedule_pending after a successful wake.  The
    // target CPU may be idle (must run immediately), running another
    // task whose quantum hasn't expired (will observe at next preempt
    // check), or mid-context-switch (will observe on the way out).
    // Getting this right is tricky under SMP, so we take the simple-
    // and-correct route: raise the flag unconditionally, and send an
    // IPI if the target is a DIFFERENT CPU.  A wasted IPI is ~500 cycles;
    // a missed wake is a stuck task.
    home->reschedule_pending = 1;
    spin_unlock_irqrestore(&home->rq_lock, flags);

    if (home != this_cpu())
        lapic_send_ipi(home->apic_id, VEC_IPI_RESCHEDULE);
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
        // Walk Chase-Lev slots [top, bottom) directly.  rq_lock
        // excludes the owner, but thieves on remote CPUs may CAS top
        // concurrently — which at worst hides a task that just got
        // stolen (still alive on the thief's CPU, just not listed
        // here this pass).  Acceptable for debug enumeration.
        for (uint8_t i = 0; i < MLFQ_LEVELS; i++) {
            chaselev_deque_t* dq = &c->rq.levels[i];
            uint64_t top = __atomic_load_n(&dq->top, __ATOMIC_ACQUIRE);
            uint64_t bot = __atomic_load_n(&dq->bottom, __ATOMIC_ACQUIRE);
            for (uint64_t j = top; (int64_t)(j - bot) < 0; j++) {
                task_t* p = (task_t*)dq->slots[j & CHASELEV_MASK];
                if (p) cb(p, data);
            }
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
