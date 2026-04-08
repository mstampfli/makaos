#include "sched.h"
#include "process.h"
#include "signal.h"
#include "timer.h"
#include "tsc.h"
#include "tss.h"
#include "common.h"

// ── MLFQ parameters ───────────────────────────────────────────────────────
// 4 priority levels.  Level 0 is highest (interactive).
// Quantum doubles at each level.  Boost every BOOST_INTERVAL ticks moves
// all tasks back to level 0, preventing starvation of CPU-bound tasks.

#define MLFQ_LEVELS      4
#define BOOST_INTERVAL   100   // ticks ≈ 1 s at 100 Hz

static const uint8_t s_quanta[MLFQ_LEVELS] = {2, 4, 8, 16};

// ── Globals ───────────────────────────────────────────────────────────────

task_t* g_current = NULL;

// Per-level FIFO queues.
static task_t* s_heads[MLFQ_LEVELS];
static task_t* s_tails[MLFQ_LEVELS];

// Singly-linked list of sleeping tasks (state == TASK_SLEEPING).
// Uses task_t::next as the link field.
static task_t* s_sleep_head = NULL;

// Singly-linked list of zombie tasks (state == TASK_ZOMBIE).
// Tasks stay here until the parent calls wait() to reap them.
static task_t* s_zombie_head = NULL;

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
    timer_register_tick(sched_tick);
}

void sched_add(task_t* proc) {
    enqueue(proc);
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
        while (!next) {
            __asm__ volatile("sti\nhlt\ncli");
            next = dequeue();
        }
    }

    task_t* prev = g_current;
    if (prev != &s_idle && prev->state == TASK_RUNNING) {
        if (preempted) {
            // Used full quantum → demote.
            if (prev->mlfq_level < MLFQ_LEVELS - 1)
                prev->mlfq_level++;
            prev->mlfq_ticks_left = s_quanta[prev->mlfq_level];
        } else {
            // Voluntary — stay at same level, refresh quantum.
            prev->mlfq_ticks_left = s_quanta[prev->mlfq_level];
        }
        enqueue(prev);
    }

    next->state = TASK_RUNNING;
    g_current   = next;

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
}

// Add a TASK_ZOMBIE task to the zombie list (called from sys_exit).
void sched_add_zombie(task_t* t) {
    t->next = s_zombie_head;
    s_zombie_head = t;
}

// Remove and return the zombie with the given pid (0 = any).
// Returns NULL if not found.  Caller must call process_destroy on result.
task_t* sched_reap_zombie(uint32_t pid) {
    task_t** pp = &s_zombie_head;
    while (*pp) {
        if (pid == 0 || (*pp)->pid == pid) {
            task_t* z = *pp;
            *pp = z->next;
            z->next = NULL;
            return z;
        }
        pp = &(*pp)->next;
    }
    return NULL;
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
        if (g_current && g_current->pid == pid) a.found = 1;
        if (!a.found) sched_for_each(find_pid_cb, &a);
        if (!a.found) return 1; // truly gone (killed without becoming zombie)
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

// ── sched_queue_head — removed: use sched_for_each instead ───────────────
