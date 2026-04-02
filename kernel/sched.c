#include "sched.h"
#include "process.h"
#include "signal.h"
#include "timer.h"
#include "tss.h"
#include "common.h"

// ── Globals ───────────────────────────────────────────────────────────────

task_t* g_current = NULL;

// FIFO run queue: processes waiting to run.
// head = next to be dequeued; tail = last enqueued.
static task_t* s_head = NULL;
static task_t* s_tail = NULL;

// Built-in idle process.  Represents kmain's execution context.
// It is NEVER placed in the run queue — it is the fallback when the queue
// is empty.  Its cpu_ctx_t is filled by the first context_switch away from it.
// In sched.c
// kernel/sched.c

// Allocate a static buffer for the idle process stack
static uint8_t s_idle_stack[PAGE_SIZE]; 

static task_t s_idle = {
    .pid        = 0,
    .tgid       = 0,
    .ppid       = 0,
    .flags      = TASK_FLAG_KTHREAD,
    .state      = TASK_RUNNING,
    .pml4_phys  = 0,
    .ctx        = {0},
    .kstack_top = (uint64_t)s_idle_stack + PAGE_SIZE,
    .next       = NULL,
};

// ── Internal helpers ──────────────────────────────────────────────────────

static void enqueue(task_t* p) {
    p->state = TASK_READY;
    p->next  = NULL;
    if (!s_tail) {
        s_head = s_tail = p;
    } else {
        s_tail->next = p;
        s_tail = p;
    }
}

static task_t* dequeue(void) {
    if (!s_head) return NULL;
    task_t* p = s_head;
    s_head = p->next;
    if (!s_head) s_tail = NULL;
    p->next = NULL;
    return p;
}

// ── Public API ────────────────────────────────────────────────────────────

void sched_init(void) {
    s_head   = NULL;
    s_tail   = NULL;
    g_current = &s_idle;

    // Register sched_tick as the timer callback.
    timer_register_tick(sched_tick);
}

void sched_add(task_t* proc) {
    enqueue(proc);
}

// Returns the head of the run queue (for signal_send_group to walk).
task_t* sched_queue_head(void) {
    return s_head;
}

// Called from timer IRQ — only sets a flag, never touches the stack.
static volatile uint8_t s_reschedule = 0;

void sched_tick(void) {
    s_reschedule = 1;
}

// Called voluntarily from process context — does the actual switch.
static void do_switch(void) {
    task_t* next = dequeue();
    if (!next) return;

    task_t* prev = g_current;
    if (prev != &s_idle && prev->state == TASK_RUNNING)
        enqueue(prev);

    next->state = TASK_RUNNING;
    g_current   = next;

    tss_set_rsp0(next->kstack_top);

    context_switch(&prev->ctx, &next->ctx, next->pml4_phys);

    // Deliver any pending signals to the task we just switched back to.
    signal_deliver_pending();

    if (prev != &s_idle && prev->state == TASK_DEAD)
        process_destroy(prev);

    __asm__ volatile("sti");
}

void sched_yield(void) {
    s_reschedule = 0;
    do_switch();
}

void sched_sleep(void) {
    if (g_current && g_current != &s_idle)
        g_current->state = TASK_SLEEPING;
    s_reschedule = 0;
    do_switch();
}

void sched_wake(task_t* proc) {
    if (!proc) return;
    if (proc->state != TASK_SLEEPING) return;
    enqueue(proc);  // sets state to TASK_READY
}
