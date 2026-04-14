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

// Pointer to the currently executing task ON THIS CPU.
//
// Under SMP this MUST be a per-CPU accessor, not a global.  Reading a
// single global from CPU A would give you whatever task CPU B most
// recently switched to — nonsense.
//
// Every read/write of g_current is rewritten by this macro to go
// through the per-CPU slot in cpu_t.  Because the result is an
// lvalue (struct field access through a pointer), existing code like
// `g_current = next;` still compiles and does the right thing.
//
// IMPORTANT: this_cpu() dereferences a per-CPU pointer, so any access
// to g_current that expects a stable value across a possible context
// switch must bracket the read with preempt_disable / preempt_enable.
// In practice that's already what every call site does (the
// scheduler itself is the only place g_current changes, and signal
// delivery etc. reads it within a syscall context where we're not
// yet preemptible).
#include "cpu.h"
#define g_current (this_cpu()->current)

// The first user process (login/init).  Orphaned children are reparented here.
extern task_t* g_init_task;

// Initialise the scheduler data structures.  Call before timer_init().
void sched_init(void);

// Add a PROC_READY process to the tail of the run queue.
void sched_add(task_t* proc);

// ── Per-task child list helpers ───────────────────────────────────────────
void task_child_add   (task_t* parent, task_t* child);
void task_child_remove(task_t* parent, task_t* child);

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
// Does NOT check parent — use sched_reap_child_zombie for waitpid.
// Returns NULL if not found.  Caller is responsible for process_destroy.
task_t* sched_reap_zombie(uint32_t pid);

// Reap a zombie that is a child of parent_pid.
// target_pid == 0: any child; target_pid != 0: that specific pid.
// Returns NULL if not found.
task_t* sched_reap_child_zombie(uint32_t parent_pid, uint32_t target_pid);

// Non-blocking: check if a zombie child of parent_pid exists.
// target_pid == 0: any child; target_pid != 0: that specific pid.
uint8_t sched_has_child_zombie(uint32_t parent_pid, uint32_t target_pid);

// Walk every task in every MLFQ level, calling cb(t, data) for each.
void sched_for_each(void (*cb)(task_t*, void*), void* data);

// Find a task by pid.  Searches run queues and zombie list.
// Returns NULL if not found.
task_t* sched_find_pid(uint32_t pid);

// ── Task index tables (pgid / tgid / sid → task list head) ──────────────
// O(1) avg lookup.  Used by signal_send_{group,pgrp} and tty_get_ctty.
// Callers walk the returned head via t->pg_next / tg_next / sid_next.
//
// Insert/remove are called from sched_add and sched_add_zombie / reap
// respectively.  Mid-life ID changes use the _changing/_changed pair.

void task_idx_insert(task_t* t);
void task_idx_remove(task_t* t);

// Call before mutating t->pgid or t->sid, then again after the mutation.
void task_idx_pgid_changing(task_t* t);
void task_idx_pgid_changed (task_t* t);
void task_idx_sid_changing (task_t* t);
void task_idx_sid_changed  (task_t* t);

// Head-of-bucket accessors.  Returned task chains through pg_next / etc.
task_t* task_idx_pgid_head(uint32_t pgid);
task_t* task_idx_tgid_head(uint32_t tgid);
task_t* task_idx_sid_head (uint32_t sid);

// Called from the timer IRQ handler after sched_tick() if s_reschedule is set.
// Performs a preemptive context switch — safe to call from IRQ context because
// context_switch saves the IRQ stub's rsp; iretq in the stub restores it later.
void sched_preempt(void);
