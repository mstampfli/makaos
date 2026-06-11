#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "kheap.h"
#include "vfs.h"
#include "sched.h"
#include "rcu.h"

// ── task_mm_t helpers ─────────────────────────────────────────────────────

task_mm_t* task_mm_alloc(phys_addr_t pml4, mm_t* mm) {
    task_mm_t* m = kmalloc(sizeof(task_mm_t));
    if (!m) return NULL;
    m->pml4_phys = pml4;
    m->mm        = mm;
    m->refs      = 1;
    m->cpu_mask  = 0;   // TLB shootdown mask: no CPU has loaded this yet
    return m;
}

void task_mm_release(task_mm_t* m) {
    if (!m) return;
    if (--m->refs > 0) return;
    // Sanity: on final release every task that referenced this mm has
    // exited or exec'd, and do_switch's mask-clear ran as they left
    // their CPUs.  If a bit is still set here it's an accounting bug,
    // not a real CPU holding the pml4 — defensive zero so no future
    // tlb_flush_range tries to IPI a freed address space.
    m->cpu_mask = 0;
    // Use VMA-aware free: skip freeing physical frames for shared VMAs
    // (shmem owns those frames — they're freed when the shmem refcount hits 0).
    vmm_free_user_ex(m->pml4_phys, m->mm);
    pmm_buddy_free(m->pml4_phys, 0);
    if (m->mm) mm_destroy(m->mm);  // also unrefs shmem objects in each VMA
    kfree(m);
}

// ── task_files_t helpers ──────────────────────────────────────────────────
#define FD_INITIAL_CAP 4

// Allocate a fresh fdtable_t with `cap` zero-filled slots.
static fdtable_t* fdtable_alloc(uint32_t cap) {
    fdtable_t* ft = kmalloc(sizeof(fdtable_t));
    if (!ft) return NULL;
    ft->fd_table = kmalloc(cap * sizeof(vfs_file_t*));
    if (!ft->fd_table) { kfree(ft); return NULL; }
    ft->fd_flags = kmalloc(cap * sizeof(uint32_t));
    if (!ft->fd_flags) { kfree(ft->fd_table); kfree(ft); return NULL; }
    __builtin_memset(ft->fd_table, 0, cap * sizeof(vfs_file_t*));
    __builtin_memset(ft->fd_flags, 0, cap * sizeof(uint32_t));
    ft->cap = cap;
    return ft;
}

// call_rcu callback: free an obsolete fdtable_t after its grace period.
// Only frees the arrays + the fdtable_t struct; it does NOT close any
// files (those were either still owned by the live table after the COW,
// or were already vfs_close'd synchronously by the caller that dropped
// the slot).
static void fdtable_free_rcu(void* data) {
    fdtable_t* ft = (fdtable_t*)data;
    kfree(ft->fd_table);
    kfree(ft->fd_flags);
    kfree(ft);
}

uint8_t fd_table_init(task_files_t* f, uint32_t cap) {
    fdtable_t* ft = fdtable_alloc(cap);
    if (!ft) return 0;
    __atomic_store_n(&f->ft, ft, __ATOMIC_RELEASE);
    return 1;
}

// Caller MUST hold f->lock.  Copy-on-write: publish a new, larger fdtable_t
// with the existing slots preserved, and hand the old one to call_rcu so
// any concurrent reader still dereferencing the old arrays stays valid
// until their RCU reader section ends.
uint8_t fd_table_grow(task_files_t* f) {
    fdtable_t* old = f->ft;
    uint32_t old_cap = old->cap;
    uint32_t new_cap = old_cap * 2;

    fdtable_t* neu = fdtable_alloc(new_cap);
    if (!neu) return 0;

    __builtin_memcpy(neu->fd_table, old->fd_table, old_cap * sizeof(vfs_file_t*));
    __builtin_memcpy(neu->fd_flags, old->fd_flags, old_cap * sizeof(uint32_t));

    __atomic_store_n(&f->ft, neu, __ATOMIC_RELEASE);
    // Expedited: fd_table_grow runs from inside open()/dup()/socket()
    // on the user-syscall path.  The first few fds in a new process
    // always hit this (FD_INITIAL_CAP=4), so login, shells, and every
    // spawn pay the grace-period wait on the critical path.
    call_rcu_expedited(fdtable_free_rcu, old);
    return 1;
}

task_files_t* task_files_alloc(void) {
    task_files_t* f = kmalloc(sizeof(task_files_t));
    if (!f) return NULL;
    f->ft   = NULL;
    f->refs = 1;
    spin_lock_init(&f->lock);
    return f;
}

void task_files_release(task_files_t* f) {
    if (!f) return;
    extern void kprintf(const char*, ...);
    uint32_t pre_refs = f->refs;
    if (--f->refs > 0) {
        kprintf("[files] release: refs %u -> %u (NOT freeing yet)\n",
                pre_refs, f->refs);
        return;
    }
    kprintf("[files] release: refs %u -> 0, closing fds\n", pre_refs);
    fdtable_t* ft = f->ft;
    int closed = 0;
    if (ft) {
        for (uint32_t i = 0; i < ft->cap; i++)
            if (ft->fd_table[i]) { vfs_close(ft->fd_table[i]); closed++; }
        kprintf("[files] closed %d fds (cap=%u)\n", closed, (unsigned)ft->cap);
        kfree(ft->fd_table);
        kfree(ft->fd_flags);
        kfree(ft);
    }
    kfree(f);
}

// ── PID pool ──────────────────────────────────────────────────────────────
// Bitmap allocation: O(1) amortised via next-fit cursor + ctzll on free words.
// 65535 PIDs = 1024 x 64-bit words = 8 KiB BSS.
// PIDs 0-9 reserved (kernel); userland starts at 10.
#define PID_MAX   65535u
#define PID_WORDS ((PID_MAX + 1 + 63u) / 64u)   // 1024 words

static uint64_t s_pid_bitmap[PID_WORDS] = { [0] = 0x3FFu }; // reserve 0-9
static uint32_t s_pid_next = 10;

uint32_t pid_alloc(void) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0) ? s_pid_next : 10u;
        uint32_t end   = (pass == 0) ? PID_MAX    : s_pid_next;
        uint32_t sw = start / 64u;
        uint32_t ew = end   / 64u;
        for (uint32_t w = sw; w <= ew; w++) {
            uint64_t free_bits = ~s_pid_bitmap[w];
            if (!free_bits) continue;
            if (w == sw) free_bits &= ~((1ull << (start % 64u)) - 1u);
            if (w == ew && (end % 64u) != 63u)
                free_bits &= (1ull << ((end % 64u) + 1u)) - 1u;
            if (!free_bits) continue;
            uint32_t b   = (uint32_t)__builtin_ctzll(free_bits);
            uint32_t pid = w * 64u + b;
            if (pid > PID_MAX) break;
            s_pid_bitmap[w] |= (1ull << b);
            s_pid_next = (pid + 1u <= PID_MAX) ? pid + 1u : 10u;
            return pid;
        }
    }
    return 0; // out of PIDs
}

void pid_free(uint32_t pid) {
    if (pid < 10u || pid > PID_MAX) return;
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
    t->pg_prev = t->pg_next = NULL;
    t->tg_prev = t->tg_next = NULL;
    t->sid_prev = t->sid_next = NULL;
    t->home_cpu = 0;
    t->last_ran_cpu = 0;
    t->mm_shared        = mm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->wake_pending     = 0;
    t->sigstate.pending = 0;
    t->sigstate.blocked = 0;
    t->sigstate.sigframe_rsp = 0;
    t->signalfd_head    = NULL;  // no signalfd subscribers until signalfd_new
    t->fs_base           = 0;    // no TLS until SYS_SET_FS
    t->drm_bytes_charged = 0;
    t->drm_priority      = 0;
    // Zero the whole handlers[] array.  task_t comes out of kmalloc
    // uninitialised, so handlers[].sa_handler could hold slab garbage.
    // On first signal delivery the kernel would treat that garbage as
    // a canonical user pointer and iretq into non-canonical memory,
    // #GP'ing the kernel.  SIG_DFL == 0 is exactly what memset gives us.
    __builtin_memset(t->sigstate.handlers, 0, sizeof(t->sigstate.handlers));
    t->umask            = 0022u; // default umask: rwxr-xr-x
    t->exit_code        = 0;
    t->sleep_until_ns   = 0;
    t->cwd = kmalloc(KPATH_MAX);
    if (t->cwd) { t->cwd[0] = '/'; t->cwd[1] = '\0'; }
    t->comm[0] = '\0'; // set by elf_load_with_argv or task_fork
    t->pf_disk  = 0;
    t->pf_cache = 0;

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
//
// Two-phase destroy to make task_t lifecycle SMP-safe:
//
// 1. Synchronous phase:
//      - Remove from pid_ht (readers that hold an already-loaded
//        pointer keep it valid; new lookups return NULL).
//      - Free the pid number so it can be reused.
//    These two steps must happen BEFORE we defer anything — they
//    release resources other subsystems might be waiting for.
//
// 2. Deferred phase (via call_rcu):
//      - Release kstack, mm, files, cwd, unveil, then kfree(t).
//    All of these touch memory that concurrent RCU readers might
//    still be dereferencing (e.g. signal_send_pgrp walking a task
//    list, reading t->sigstate).  Deferring until the next RCU
//    grace period guarantees every reader that started before
//    pid_ht_remove has finished.
//
// On UP, synchronize_rcu is a no-op so call_rcu is effectively
// synchronous with a function-call overhead.  Under SMP the task
// survives until every CPU has passed through a quiescent state.

static void task_free_rcu(void* data) {
    task_t* t = (task_t*)data;
    kstack_free(t->kstack_top);
    task_mm_release(t->mm_shared);
    task_files_release(t->files_shared);
    kfree(t->cwd);
    unveil_free(&t->unveil);
    kfree(t);
}

void task_destroy(task_t* t) {
    if (!t) return;
    // Remove from pid_ht first.  Zombies STAY in pid_ht until this
    // final destroy, so every specific-pid lookup is O(1) for
    // zombies AND living tasks.
    pid_ht_remove(t);
    pid_free(t->pid);

    // Defer the rest of the free via RCU.  Concurrent readers in
    // pid_ht_find / signal_send_pgrp / sched_for_each etc. may hold
    // a task_t pointer from a recent lookup — they must be allowed
    // to finish before the storage disappears.
    //
    // Expedited grace period: task_destroy runs inside do_switch's
    // tail, after we've already context-switched INTO the next task.
    // That "next" task is otherwise stalled until the grace period
    // closes — per-process reap latency directly visible as the
    // trailing delay after ls / ps / short shells exit.  User
    // confirmed classic vs expedited here makes no difference to
    // the separate `ls` hang (which is pre-existing and unrelated
    // to the reap path), so expedited stays as the better default.
    call_rcu_expedited(task_free_rcu, t);
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
    // Snapshot the parent's fdtable_t (outside RCU is fine — fork runs
    // in the parent's own task context so its own table can't be
    // concurrently torn down).
    fdtable_t* pft = parent->files_shared->ft;
    fd_table_init(files, pft->cap);
    fdtable_t* cft = files->ft;
    for (uint32_t i = 0; i < pft->cap; i++) {
        cft->fd_table[i] = vfs_dup(pft->fd_table[i]);
        cft->fd_flags[i] = pft->fd_flags[i];
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
    t->pg_prev = t->pg_next = NULL;
    t->tg_prev = t->tg_next = NULL;
    t->sid_prev = t->sid_next = NULL;
    t->home_cpu = 0;
    t->last_ran_cpu = 0;
    t->mm_shared        = tmm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->wake_pending     = 0;
    t->sigstate.pending = 0;
    // fork(): inherit the parent's signal mask and sigactions, matching
    // POSIX.  exec() later clears user handlers back to SIG_DFL (see
    // sys_exec).  Copy the whole handlers array so sa_mask/sa_flags
    // come with; signum 0 is never used but copying it is free.
    t->sigstate.blocked = parent->sigstate.blocked;
    t->sigstate.sigframe_rsp = 0;
    __builtin_memcpy(t->sigstate.handlers,
                     parent->sigstate.handlers,
                     sizeof(t->sigstate.handlers));
    // signalfd subscribers are per-task; child starts with none (parent's
    // signalfds stay on the parent).  Linux fork() does the same — fds
    // are duplicated in the fd table but each signalfd's wait_queue list
    // is its owner-specific subscriber membership.
    t->signalfd_head     = NULL;
    t->fs_base           = parent->fs_base;  // same address space → same TLS block
    t->drm_bytes_charged = 0;   // fresh tasks start with no DRM charges
    t->drm_priority      = parent->drm_priority;   // inherit priority
    t->cwd = kmalloc(KPATH_MAX);
    if (t->cwd) __builtin_memcpy(t->cwd, parent->cwd, KPATH_MAX);
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
    __builtin_memcpy(t->ctx.fxsave_buf, parent->ctx.fxsave_buf,
                     sizeof(t->ctx.fxsave_buf));

    return t;
}
