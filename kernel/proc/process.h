#pragma once
#include "common.h"
#include "vmm.h"
#include "mm.h"
#include "signal.h"
#include "tss.h"
#include "vfs.h"
#include "cred.h"
#include "pledge.h"
#include "unveil.h"

// ── Task flags ────────────────────────────────────────────────────────────
#define TASK_FLAG_KTHREAD   (1U << 0)  // kernel thread: runs at CPL=0
#define TASK_FLAG_THREAD    (1U << 1)  // user thread: shares at least one of mm/files

// ── Thread creation flags (sys_thread) ───────────────────────────────────
#define THREAD_SHARE_MM    (1U << 0)  // share address space (pml4 + mm)
#define THREAD_SHARE_FILES (1U << 1)  // share fd table

// ── Task state ────────────────────────────────────────────────────────────
typedef enum {
    TASK_READY    = 0,
    TASK_RUNNING  = 1,
    TASK_DEAD     = 2,
    TASK_SLEEPING = 3,
    TASK_ZOMBIE   = 4,  // exited, waiting for parent to reap via wait()
} task_state_t;

#define PROC_READY    TASK_READY
#define PROC_RUNNING  TASK_RUNNING
#define PROC_DEAD     TASK_DEAD
#define PROC_SLEEPING TASK_SLEEPING
typedef task_state_t proc_state_t;

// ── Saved CPU context ─────────────────────────────────────────────────────
// The fxsave_buf must be 16-byte aligned (required by FXSAVE/FXRSTOR).
// We put it after the integer fields; the struct itself is always allocated
// via kmalloc which returns 8-byte-aligned memory, so we use __attribute__
// aligned to ensure the buffer starts on a 16-byte boundary.
typedef struct __attribute__((aligned(16))) {
    uint64_t rsp, rbx, rbp, r12, r13, r14, r15;
    uint64_t _fxsave_pad;           // pad to 16-byte align fxsave_buf (7×8=56, +8=64)
    uint8_t  fxsave_buf[512];       // FXSAVE state: x87 + SSE registers
} cpu_ctx_t;

// ── Shared address space (ref-counted) ───────────────────────────────────
// All tasks sharing the same virtual address space point to one task_mm_t.
// Freed when refs drops to 0.
typedef struct {
    phys_addr_t pml4_phys;
    mm_t*       mm;         // VMA list + brk; NULL for kernel threads
    uint32_t    refs;
} task_mm_t;

// ── Shared file descriptor table (ref-counted) ───────────────────────────
// All tasks sharing the same fd table point to one task_files_t.
// Freed when refs drops to 0.
// ── fd-descriptor flags (per-descriptor, not per-file-description) ────────
// FD_CLOEXEC: close this fd on execve.
#define FD_CLOEXEC  1u

// ── Shared file descriptor table (ref-counted) ────────────────────────────
// fd_table[i]  — open file description (NULL = closed)
// fd_flags[i]  — per-descriptor flags (FD_CLOEXEC etc.)
// Both arrays are fd_capacity entries long and always grown together.
typedef struct {
    vfs_file_t** fd_table;
    uint32_t*    fd_flags;    // parallel array: FD_CLOEXEC per descriptor
    uint32_t     fd_capacity;
    uint32_t     refs;
} task_files_t;

// ── Task Control Block ────────────────────────────────────────────────────
typedef struct __attribute__((aligned(16))) task_t {
    uint32_t      pid;
    uint32_t      tgid;     // thread group ID (== pid for single-threaded)
    uint32_t      ppid;
    uint32_t      pgid;     // process group ID (job control)
    uint32_t      sid;      // session ID
    uint32_t      flags;    // TASK_FLAG_*

    task_state_t  state;

    task_mm_t*    mm_shared;     // ref-counted address space (never NULL)
    task_files_t* files_shared;  // ref-counted fd table    (never NULL)

    cpu_ctx_t     ctx;
    virt_addr_t   kstack_top;

    sigstate_t    sigstate;

    int32_t       exit_code;    // set by sys_exit, readable via wait()

    uint64_t      sleep_until_ns; // wake time for nanosleep (TSC nanoseconds)

    uint16_t      umask;        // file creation mask (default 022)


    uint8_t       mlfq_level;
    uint8_t       mlfq_ticks_left;
    uint32_t      preempt_depth;   // preemption disable nesting counter

    char*         cwd;          // current working directory (absolute path, heap-allocated KPATH_MAX)
    char          comm[16];     // short process name (basename of argv[0], NUL-terminated)

    // ── Security ──────────────────────────────────────────────────────────
    cred_t        cred;         // uid/gid/euid/egid/suid/sgid + supplemental groups
    uint32_t      pledge_mask;  // PLEDGE_* bitmask; starts as PLEDGE_ALL (no restriction)
    unveil_table_t unveil;      // per-process filesystem view restriction

    struct task_t* next;        // scheduler / sleep / zombie list link

    // ── Per-task child list ───────────────────────────────────────────────
    // children: singly-linked list of direct children (head pointer).
    // child_next: this task's link in its parent's children list.
    struct task_t* children;    // head of direct-children list
    struct task_t* child_next;  // next sibling in parent's children list

    // ── Task index lists (pgid/tgid/sid hash tables) ─────────────────────
    // Each task is in three intrusive doubly-linked lists maintained by
    // sched.c: one per {pgid, tgid, sid} hash-bucket membership.  Used so
    // signal_send_pgrp/group and tty_get_ctty are O(list length) instead
    // of O(total tasks in system).
    //
    // Locking: today these are protected by the global scheduler lock
    // (single CPU).  Under SMP, pgid/tgid/sid hash tables grow their own
    // spinlock_t locks (see smp.h).
    struct task_t* pg_prev;
    struct task_t* pg_next;
    struct task_t* tg_prev;
    struct task_t* tg_next;
    struct task_t* sid_prev;
    struct task_t* sid_next;
} task_t;

typedef task_t process_t;

// ── task_mm_t / task_files_t lifecycle ───────────────────────────────────
task_mm_t*    task_mm_alloc(phys_addr_t pml4, mm_t* mm);
void          task_mm_release(task_mm_t* m);
task_files_t* task_files_alloc(void);
void          task_files_release(task_files_t* f);

// ── API ───────────────────────────────────────────────────────────────────

task_t* task_create_kthread(void (*entry)(void), uint32_t pid);
task_t* task_create_user(phys_addr_t code_phys, uint32_t code_pages, uint32_t pid);

static inline task_t* process_create(void (*entry)(void), uint32_t pid) {
    return task_create_kthread(entry, pid);
}
static inline task_t* process_create_user(phys_addr_t cp, uint32_t cn, uint32_t pid) {
    return task_create_user(cp, cn, pid);
}

void    task_destroy(task_t* t);
static inline void process_destroy(task_t* t) { task_destroy(t); }

mm_t*   task_get_mm(void* task);

void context_switch(cpu_ctx_t* from, cpu_ctx_t* to, phys_addr_t new_pml4);

extern void proc_trampoline(void);
extern void user_trampoline(void);
extern void fork_trampoline(void);

// PID pool.
uint32_t pid_alloc(void);
void     pid_free(uint32_t pid);

// fd table helpers — operate on task_files_t directly.
uint8_t fd_table_init(task_files_t* f, uint32_t cap);
uint8_t fd_table_grow(task_files_t* f);

// Fork the current user task.
// user_rbp/rbx/r12-r15 are the callee-saved registers at the syscall site;
// they must be restored in the child so it returns to user space correctly.
task_t* task_fork(task_t* parent, uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp,
                  uint64_t user_rbp, uint64_t user_rbx,
                  uint64_t user_r12, uint64_t user_r13, uint64_t user_r14, uint64_t user_r15);
