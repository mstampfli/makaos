#pragma once
#include "process.h"

// ── Scheduler ─────────────────────────────────────────────────────────────
// Simple preemptive round-robin scheduler.
//
// Run queue: FIFO singly-linked list of PROC_READY processes.
//   • sched_add()  enqueues at the tail.
//   • sched_tick() dequeues from the head, re-enqueues the previous process.
//
// Idle process: if the queue is empty on a tick, the scheduler keeps running
// the current process (or falls back to the built-in idle loop in kmain).

// Pointer to the currently executing process.
// NULL means the idle process (kmain's hlt loop) is running.
extern task_t* g_current;

// Initialise the scheduler data structures.  Call before timer_init().
void sched_init(void);

// Add a PROC_READY process to the tail of the run queue.
void sched_add(task_t* proc);

// Called on every timer tick (by timer_irq_handler via timer_register_tick).
// Picks the next PROC_READY process and context-switches into it.
void sched_tick(void);

// Voluntarily yield the rest of the current time slice.
// Can be called from any process.
void sched_yield(void);

// Mark the current process PROC_SLEEPING and switch away.
// The process will not run again until sched_wake() is called on it.
void sched_sleep(void);

// Wake a sleeping process: mark it PROC_READY and enqueue it.
// Safe to call from IRQ handlers.
// Does nothing if the process is not PROC_SLEEPING.
void sched_wake(task_t* proc);

// Returns the head of the ready run queue (used by signal_send_group).
task_t* sched_queue_head(void);
