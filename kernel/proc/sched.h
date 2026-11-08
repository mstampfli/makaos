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

// Build a per-CPU idle task for g_cpus[id] and install it as that CPU's
// `current` + `idle` sentinel.  sched_init() calls this for the BSP;
// cpu_init_ap() calls it for each AP as it comes online.
void sched_init_idle_for_cpu(uint32_t id);

// Enter the idle loop on the current CPU.  Called once from cpu_init_ap
// as the AP's final transition from "bring-up C code" into "I'm running
// the idle task".  Never returns; on every hlt wake it re-checks the
// runqueue via sched_yield and dispatches any work that got enqueued.
void sched_enter_idle(void) __attribute__((noreturn));

// Add a PROC_READY process to the tail of the run queue.
void sched_add(task_t* proc);

// ── Per-task child list helpers ───────────────────────────────────────────
//
// Lock-free Treiber stack.  See task_child_add's commentary in sched.c
// for the full design and the memory-ordering contract.

// Prepend one child to parent's list.  One CAS.  Safe from any CPU.
void task_child_add(task_t* parent, task_t* child);

// Prepend an entire pre-linked chain (head..tail via child_next) onto
// parent's list in one CAS.  Used by sys_exit to batch-reparent every
// orphan onto g_init_task.
void task_child_add_chain(task_t* parent, task_t* head, task_t* tail);

// Drain parent's children list, look for a zombie matching target_pid
// (0 = any).  Returns the reaped child (unlinked) or NULL.  Splices
// survivors back.  *out_found (optional) is set iff at least one child
// matched target_pid regardless of state — used by sys_wait to
// distinguish ECHILD from "wait and retry".
task_t* task_children_reap(task_t* parent, uint32_t target_pid,
                            uint8_t* out_found);

// Move every child of `from` onto `to`'s list, updating ppid to
// `to->pid` in the process.  Used by the exit / fatal-signal paths.
void task_children_reparent(task_t* from, task_t* to);
// Atomically clear a task's children list (no reparent target: the task is
// init, or init is absent).  Must be atomic -- init->children is concurrently
// CAS-prepended by other exiting tasks reparenting orphans.
void task_children_clear(task_t* t);

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

// Add a zombie task to the zombie list (call from sys_exit before yielding).
void sched_add_zombie(task_t* t);

// Remove and return the zombie with the given pid (0 = any child).
// Returns NULL if not found.  Caller is responsible for process_destroy.
// sys_wait goes through task_children_reap (children list walk) and
// calls sched_reap_zombie with the exact pid to unlink it from the
// per-CPU zombie list.
task_t* sched_reap_zombie(uint32_t pid);

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

// Locked walker: invoke cb(t, data) for every task in the given
// bucket.  The table lock is held across the whole walk so
// concurrent insert / remove cannot tear the list, and the callback
// sees a consistent snapshot.
//
// The callback MUST be short and MUST NOT take any lock that a
// task_idx writer could be waiting on.  signal_send is safe
// (signal_send → atomic bit + sched_wake → rq_lock; rq_lock is
// lower in the ordering than task_idx locks, no cycle).
void task_idx_pgid_walk(uint32_t pgid, void (*cb)(task_t*, void*), void* data);
void task_idx_tgid_walk(uint32_t tgid, void (*cb)(task_t*, void*), void* data);

// Called from the timer IRQ handler after sched_tick() if s_reschedule is set.
// Performs a preemptive context switch — safe to call from IRQ context because
// context_switch saves the IRQ stub's rsp; iretq in the stub restores it later.
void sched_preempt(void);

// ── PID hash table API (RCU-protected) ─────────────────────────────────
// pid_ht_insert is called from sched_add on task creation.
// pid_ht_remove is called from task_destroy on final reap — zombies
// STAY in the table until destroy, so every specific-pid lookup is
// O(1) for living tasks AND zombies.
// pid_ht_find takes zero locks (RCU reader section inside).
void     pid_ht_insert(task_t* t);
void     pid_ht_remove(task_t* t);
task_t*  pid_ht_find  (uint32_t pid);
