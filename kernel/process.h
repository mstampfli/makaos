#pragma once
#include "common.h"
#include "vmm.h"
#include "mm.h"
#include "signal.h"
#include "tss.h"
#include "vfs.h"

// ── Task flags ────────────────────────────────────────────────────────────
#define TASK_FLAG_KTHREAD  (1U << 0)  // kernel thread: runs at CPL=0
#define TASK_FLAG_THREAD   (1U << 1)  // user thread: shares at least one of mm/files

// ── Thread creation flags (sys_thread) ───────────────────────────────────
#define THREAD_SHARE_MM    (1U << 0)  // share address space (pml4 + mm)
#define THREAD_SHARE_FILES (1U << 1)  // share fd table

// ── Task state ────────────────────────────────────────────────────────────
typedef enum {
    TASK_READY    = 0,
    TASK_RUNNING  = 1,
    TASK_DEAD     = 2,
    TASK_SLEEPING = 3,
} task_state_t;

#define PROC_READY    TASK_READY
#define PROC_RUNNING  TASK_RUNNING
#define PROC_DEAD     TASK_DEAD
#define PROC_SLEEPING TASK_SLEEPING
typedef task_state_t proc_state_t;

// ── Saved CPU context ─────────────────────────────────────────────────────
typedef struct {
    uint64_t rsp, rbx, rbp, r12, r13, r14, r15;
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
typedef struct {
    vfs_file_t** fd_table;
    uint32_t     fd_capacity;
    uint32_t     refs;
} task_files_t;

// ── Task Control Block ────────────────────────────────────────────────────
typedef struct task_t {
    uint32_t      pid;
    uint32_t      tgid;     // thread group ID
    uint32_t      ppid;
    uint32_t      flags;    // TASK_FLAG_*

    task_state_t  state;

    task_mm_t*    mm_shared;     // ref-counted address space (never NULL)
    task_files_t* files_shared;  // ref-counted fd table    (never NULL)

    cpu_ctx_t     ctx;
    virt_addr_t   kstack_top;

    sigstate_t    sigstate;

    uint8_t       mlfq_level;
    uint8_t       mlfq_ticks_left;

    struct task_t* next;
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
task_t* task_fork(task_t* parent, uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp);
