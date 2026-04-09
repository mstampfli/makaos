#pragma once
#include "process.h"

// ── Scheduler — Multi-Level Feedback Queue (MLFQ) ────────────────────────
// 4 priority levels (0 = highest).  Quanta: 2, 4, 8, 16 ticks (100 Hz PIT).
//
// Rules:
//   • New tasks start at level 0 with a full quantum.
//   • Task uses full quantum  → demoted one level, gets new quantum.
//   • Task yields/sleeps early → stays at same level, quantum refreshed.
//   • Every BOOST_INTERVAL ticks all tasks are moved back to level 0
//     to prevent starvation of CPU-bound tasks.
//
// Idle task: if all queues are empty the idle task runs (hlt loop).

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

// Block current task until a zombie with the given pid exists (0=any).
// Returns 1 when zombie is ready, 1 if already gone.
uint8_t sched_wait_pid(uint32_t pid);

// Non-blocking: returns 1 if a zombie with the given pid (0=any) is ready.
uint8_t sched_poll_pid(uint32_t pid);

// Add a zombie task to the zombie list (call from sys_exit before yielding).
void sched_add_zombie(task_t* t);

// Remove and return the zombie with the given pid (0 = any child).
// Returns NULL if not found.  Caller is responsible for process_destroy.
task_t* sched_reap_zombie(uint32_t pid);

// Walk every task in every MLFQ level, calling cb(t, data) for each.
void sched_for_each(void (*cb)(task_t*, void*), void* data);

// Find a task by pid.  Searches run queues and zombie list.
// Returns NULL if not found.
task_t* sched_find_pid(uint32_t pid);

// Called from the timer IRQ handler after sched_tick() if s_reschedule is set.
// Performs a preemptive context switch — safe to call from IRQ context because
// context_switch saves the IRQ stub's rsp; iretq in the stub restores it later.
void sched_preempt(void);
