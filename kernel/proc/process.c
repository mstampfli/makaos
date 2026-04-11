#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "kheap.h"
#include "vfs.h"
#include "sched.h"

// ── task_mm_t helpers ─────────────────────────────────────────────────────

task_mm_t* task_mm_alloc(phys_addr_t pml4, mm_t* mm) {
    task_mm_t* m = kmalloc(sizeof(task_mm_t));
    if (!m) return NULL;
    m->pml4_phys = pml4;
    m->mm        = mm;
    m->refs      = 1;
    return m;
}

void task_mm_release(task_mm_t* m) {
    if (!m) return;
    if (--m->refs > 0) return;
    // Use VMA-aware free: skip freeing physical frames for shared VMAs
    // (shmem owns those frames — they're freed when the shmem refcount hits 0).
    vmm_free_user_ex(m->pml4_phys, m->mm);
    pmm_buddy_free(m->pml4_phys, 0);
    if (m->mm) mm_destroy(m->mm);  // also unrefs shmem objects in each VMA
    kfree(m);
}

// ── task_files_t helpers ──────────────────────────────────────────────────
#define FD_INITIAL_CAP 4

uint8_t fd_table_init(task_files_t* f, uint32_t cap) {
    f->fd_table = kmalloc(cap * sizeof(vfs_file_t*));
    if (!f->fd_table) return 0;
    f->fd_flags = kmalloc(cap * sizeof(uint32_t));
    if (!f->fd_flags) { kfree(f->fd_table); f->fd_table = NULL; return 0; }
    for (uint32_t i = 0; i < cap; i++) { f->fd_table[i] = NULL; f->fd_flags[i] = 0; }
    f->fd_capacity = cap;
    return 1;
}

uint8_t fd_table_grow(task_files_t* f) {
    uint32_t new_cap = f->fd_capacity * 2;

    vfs_file_t** tbl = kmalloc(new_cap * sizeof(vfs_file_t*));
    if (!tbl) return 0;
    uint32_t* flg = kmalloc(new_cap * sizeof(uint32_t));
    if (!flg) { kfree(tbl); return 0; }

    for (uint32_t i = 0; i < f->fd_capacity; i++) {
        tbl[i] = f->fd_table[i];
        flg[i] = f->fd_flags[i];
    }
    for (uint32_t i = f->fd_capacity; i < new_cap; i++) {
        tbl[i] = NULL;
        flg[i] = 0;
    }

    kfree(f->fd_table);
    kfree(f->fd_flags);
    f->fd_table    = tbl;
    f->fd_flags    = flg;
    f->fd_capacity = new_cap;
    return 1;
}

task_files_t* task_files_alloc(void) {
    task_files_t* f = kmalloc(sizeof(task_files_t));
    if (!f) return NULL;
    f->fd_table    = NULL;
    f->fd_flags    = NULL;
    f->fd_capacity = 0;
    f->refs        = 1;
    return f;
}

void task_files_release(task_files_t* f) {
    if (!f) return;
    if (--f->refs > 0) return;
    for (uint32_t i = 0; i < f->fd_capacity; i++)
        if (f->fd_table[i]) vfs_close(f->fd_table[i]);
    kfree(f->fd_table);
    kfree(f->fd_flags);
    kfree(f);
}

// ── PID pool ──────────────────────────────────────────────────────────────
#define PID_MAX   4095u
#define PID_WORDS ((PID_MAX + 1 + 63u) / 64u)

static uint64_t s_pid_bitmap[PID_WORDS] = { [0] = 0x3FFu };
static uint32_t s_pid_next = 10;

uint32_t pid_alloc(void) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? s_pid_next : 10u;
        uint32_t end   = (pass == 0) ? PID_MAX    : s_pid_next;
        for (uint32_t pid = start; pid <= end; pid++) {
            uint32_t w = pid / 64u, b = pid % 64u;
            if (!(s_pid_bitmap[w] & (1ull << b))) {
                s_pid_bitmap[w] |= (1ull << b);
                s_pid_next = (pid + 1 <= PID_MAX) ? pid + 1 : 10u;
                return pid;
            }
        }
    }
    return 0;
}

void pid_free(uint32_t pid) {
    if (pid < 10 || pid > PID_MAX) return;
    s_pid_bitmap[pid / 64u] &= ~(1ull << (pid % 64u));
}

// ── Common task init ──────────────────────────────────────────────────────

static void task_init_common(task_t* t, uint32_t pid, uint32_t flags,
                              task_mm_t* mm, task_files_t* files) {
    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = g_current ? g_current->pid : 0;
    t->pgid             = pid;   // new task is its own process group leader
    t->sid              = pid;   // and its own session leader
    t->flags            = flags;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->children         = NULL;
    t->child_next       = NULL;
    t->mm_shared        = mm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.head    = 0;
    t->sigstate.tail    = 0;
    t->sigstate.blocked = 0;
    t->umask            = 0022u; // default umask: rwxr-xr-x
    t->exit_code        = 0;
    t->sleep_until_ns   = 0;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->comm[0] = '\0'; // set by elf_load_with_argv or task_fork

    // Security: always start with root credentials and full permissions.
    // fork() (task_fork) explicitly copies cred/pledge/unveil from the parent
    // after calling task_init_common, overriding these defaults.
    // task_create_kthread and task_create_user (spawn path) start unrestricted
    // and gain restrictions via sys_pledge/sys_unveil or credential drops in
    // the process itself (e.g. login dropping from uid=0 to uid=N).
    cred_init_root(&t->cred);
    t->pledge_mask = PLEDGE_ALL;
    unveil_init(&t->unveil);

    // Initialize fxsave_buf with a valid FPU state.
    // fxrstor on a zero buffer is invalid on real CPUs (KVM) — FCW must be set.
    // Capture the current FPU state (which has a valid FCW=0x037F from UEFI/boot).
    __asm__ volatile("fxsave %0" : "=m"(t->ctx.fxsave_buf));
}

// ── task_create_kthread ───────────────────────────────────────────────────
task_t* task_create_kthread(void (*entry)(void), uint32_t pid) {
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return NULL;

    task_mm_t* mm = task_mm_alloc(vmm_alloc_pml4(), NULL);
    if (!mm) { kfree(t); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(mm); kfree(t); return NULL; }
    fd_table_init(files, FD_INITIAL_CAP);
    // Kernel threads have no stdio — fd 0/1/2 are NULL.
    // They don't do userspace I/O; any kernel-side printing goes direct to fb.

    task_init_common(t, pid, TASK_FLAG_KTHREAD, mm, files);

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;
    *(--stk) = (uint64_t)proc_trampoline;
    *(--stk) = 0; *(--stk) = 0;         // rbx, rbp
    *(--stk) = (uint64_t)entry;          // r12
    *(--stk) = 0; *(--stk) = 0; *(--stk) = 0; // r13, r14, r15
    t->ctx.rsp = (uint64_t)stk;

    return t;
}

// ── task_create_user ──────────────────────────────────────────────────────
task_t* task_create_user(phys_addr_t code_phys, uint32_t code_pages, uint32_t pid) {
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return NULL;

    phys_addr_t pml4 = vmm_alloc_pml4();
    mm_t* mm = mm_create();
    task_mm_t* tmm = task_mm_alloc(pml4, mm);
    if (!tmm) { kfree(t); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(tmm); kfree(t); return NULL; }
    fd_table_init(files, FD_INITIAL_CAP);
    // fd 0/1/2 are NULL — raw blob processes have no TTY by default.
    // Caller assigns stdio if needed (e.g. elf_load_with_argv opens tty0
    // for the sys_spawn path; fork inherits from parent).

    virt_addr_t code_start = VMM_USER_CODE_BASE;
    virt_addr_t code_end   = code_start + (virt_addr_t)code_pages * PAGE_SIZE;
    mm_vma_add(mm, code_start, code_end, VMA_R | VMA_W | VMA_X);
    for (uint32_t i = 0; i < code_pages; i++)
        vmm_page_map(pml4, code_start + (virt_addr_t)i * PAGE_SIZE,
                     code_phys + (phys_addr_t)i * PAGE_SIZE,
                     mm_vma_pte_flags(VMA_R | VMA_W | VMA_X));

    virt_addr_t stack_end   = VMM_USER_STACK_TOP;
    virt_addr_t stack_start = stack_end - VMM_USER_STACK_PAGES * PAGE_SIZE;
    mm_vma_add(mm, stack_start, stack_end, VMA_R | VMA_W | VMA_ANON);

    virt_addr_t heap_start = (code_end + PAGE_SIZE - 1) & ~PAGE_MASK;
    mm->brk_start = heap_start;
    mm->brk       = heap_start;

    task_init_common(t, pid, 0, tmm, files);

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;
    *(--stk) = (uint64_t)user_trampoline;
    *(--stk) = 0; *(--stk) = 0;      // rbx, rbp
    *(--stk) = VMM_USER_CODE_BASE;   // r12
    *(--stk) = VMM_USER_STACK_TOP;   // r13
    *(--stk) = 0; *(--stk) = 0;      // r14, r15
    t->ctx.rsp = (uint64_t)stk;

    return t;
}

// ── task_get_mm ───────────────────────────────────────────────────────────
mm_t* task_get_mm(void* task) {
    return ((task_t*)task)->mm_shared->mm;
}

// ── task_destroy ──────────────────────────────────────────────────────────
void task_destroy(task_t* t) {
    if (!t) return;
    pid_free(t->pid);
    kstack_free(t->kstack_top);
    task_mm_release(t->mm_shared);
    task_files_release(t->files_shared);
    kfree(t);
}

// ── task_fork ─────────────────────────────────────────────────────────────
task_t* task_fork(task_t* parent, uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp,
                  uint64_t user_rbp, uint64_t user_rbx,
                  uint64_t user_r12, uint64_t user_r13, uint64_t user_r14, uint64_t user_r15) {
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return NULL;

    // Fork = new process: deep-copy both mm and files.
    phys_addr_t new_pml4 = vmm_alloc_pml4();
    if (new_pml4 == PMM_INVALID_ADDR) { kfree(t); return NULL; }

    if (!vmm_clone_user_ex(new_pml4, parent->mm_shared->pml4_phys,
                           parent->mm_shared->mm)) {
        vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0); kfree(t);
        return NULL;
    }

    mm_t* new_mm = mm_clone(parent->mm_shared->mm);
    if (!new_mm) {
        vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0); kfree(t);
        return NULL;
    }

    task_mm_t* tmm = task_mm_alloc(new_pml4, new_mm);
    if (!tmm) {
        mm_destroy(new_mm); vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0); kfree(t);
        return NULL;
    }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(tmm); kfree(t); return NULL; }
    fd_table_init(files, parent->files_shared->fd_capacity);
    for (uint32_t i = 0; i < parent->files_shared->fd_capacity; i++) {
        // dup each open file description so both parent and child share it
        files->fd_table[i] = vfs_dup(parent->files_shared->fd_table[i]);
        files->fd_flags[i] = parent->files_shared->fd_flags[i];
    }

    t->pid              = pid_alloc();
    t->tgid             = t->pid;
    t->ppid             = parent->pid;
    t->pgid             = parent->pgid;  // inherit process group
    t->sid              = parent->sid;   // inherit session
    t->umask            = parent->umask; // inherit umask
    t->flags            = 0;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->children         = NULL;
    t->child_next       = NULL;
    t->mm_shared        = tmm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.head    = 0;
    t->sigstate.tail    = 0;
    t->sigstate.blocked = 0;
    for (int _i = 0; _i < 256; _i++) t->cwd[_i]  = parent->cwd[_i];
    for (int _i = 0; _i < 16;  _i++) t->comm[_i] = parent->comm[_i];
    cred_copy(&t->cred, &parent->cred);
    t->pledge_mask = parent->pledge_mask;
    unveil_copy(&t->unveil, &parent->unveil);

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = user_rsp;
    *(--stk) = user_rflags;
    *(--stk) = user_rip;
    *(--stk) = (uint64_t)fork_trampoline;
    // context_switch pops: r15, r14, r13, r12, rbp, rbx (in that order).
    // Restore the parent's user callee-saved regs so the child returns correctly.
    *(--stk) = user_rbx;
    *(--stk) = user_rbp;
    *(--stk) = user_r12;
    *(--stk) = user_r13;
    *(--stk) = user_r14;
    *(--stk) = user_r15;
    t->ctx.rsp = (uint64_t)stk;

    // Copy parent's FPU/SSE state so fxrstor on the child doesn't #GP
    // on invalid MXCSR bits from uninitialized memory.
    for (int _i = 0; _i < 512; _i++)
        t->ctx.fxsave_buf[_i] = parent->ctx.fxsave_buf[_i];

    return t;
}
