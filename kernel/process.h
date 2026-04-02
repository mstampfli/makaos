#pragma once
#include "common.h"
#include "vmm.h"
#include "mm.h"
#include "signal.h"
#include "tss.h"
#include "vfs.h"

// ── Task flags ────────────────────────────────────────────────────────────
#define TASK_FLAG_KTHREAD  (1U << 0)  // kernel thread: shares kernel PML4, no user space

// ── Task state ────────────────────────────────────────────────────────────
typedef enum {
    TASK_READY    = 0,
    TASK_RUNNING  = 1,
    TASK_DEAD     = 2,
    TASK_SLEEPING = 3,
} task_state_t;

// Keep old names as aliases so nothing else needs changing yet.
#define PROC_READY    TASK_READY
#define PROC_RUNNING  TASK_RUNNING
#define PROC_DEAD     TASK_DEAD
#define PROC_SLEEPING TASK_SLEEPING
typedef task_state_t proc_state_t;

// ── Saved CPU context ─────────────────────────────────────────────────────
typedef struct {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} cpu_ctx_t;

// ── Task Control Block (Linux: task_struct) ───────────────────────────────
typedef struct task_t {
    uint32_t      pid;         // unique ID for this task (thread ID)
    uint32_t      tgid;        // thread group ID — same as pid for the main/only thread;
                               // shared by all threads of the same process
    uint32_t      ppid;        // parent's tgid (0 for kernel threads / init)
    uint32_t      flags;       // TASK_FLAG_* bitmask

    task_state_t  state;

    phys_addr_t   pml4_phys;   // address space; kernel threads share the kernel PML4
    mm_t*         mm;          // VMA list + brk; NULL for kernel threads

    cpu_ctx_t     ctx;         // saved registers (filled by context_switch)
    virt_addr_t   kstack_top;  // top of kernel stack (passed to kstack_free on destroy)

    // Signal state.
    sigstate_t    sigstate;    // pending/blocked signal bitmasks

    // File descriptor table: fd_table[fd] = open file, NULL = closed.
    // fd 0 = stdin, fd 1 = stdout, fd 2 = stderr (set up by task_create_*).
    vfs_file_t*   fd_table[VFS_MAX_FDS];

    struct task_t* next;       // intrusive run-queue link
} task_t;

// Backwards-compat alias — all existing code using process_t still compiles.
typedef task_t process_t;

// ── API ───────────────────────────────────────────────────────────────────

// Create a kernel thread.  Shares the kernel PML4, runs at CPL=0.
task_t* task_create_kthread(void (*entry)(void), uint32_t pid);

// Create a user process.  Gets its own PML4, starts at ring 3.
task_t* task_create_user(phys_addr_t code_phys, uint32_t code_pages, uint32_t pid);

// Backwards-compat wrappers.
static inline task_t* process_create(void (*entry)(void), uint32_t pid) {
    return task_create_kthread(entry, pid);
}
static inline task_t* process_create_user(phys_addr_t cp, uint32_t cn, uint32_t pid) {
    return task_create_user(cp, cn, pid);
}

void task_destroy(task_t* task);
static inline void process_destroy(task_t* t) { task_destroy(t); }

// Returns the mm_t* for a task given an opaque pointer (used by vmm.c
// to avoid a circular header dependency).
mm_t* task_get_mm(void* task);

// Context switch (process_ctx_switch.asm).
void context_switch(cpu_ctx_t* from, cpu_ctx_t* to, phys_addr_t new_pml4);

extern void proc_trampoline(void);
extern void user_trampoline(void);
