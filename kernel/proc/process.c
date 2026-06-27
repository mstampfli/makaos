#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "kheap.h"
#include "vfs.h"
#include "sched.h"
#include "signal.h"
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
    // ATOMIC: threads sharing this mm inc/dec refs from multiple CPUs
    // concurrently (sys_thread create vs task exit).  A non-atomic RMW
    // could lose an increment, drop refs to 0 early, and free the whole
    // address space while the process is still running on it — arbitrary
    // heap corruption.  ACQ_REL so the teardown happens-after every use.
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
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

// Clone src's fd table into dst (a fresh task_files_t) for fork / thread-copy /
// spawn, taking a NEW reference (vfs_dup) on each open file.  Runs UNDER
// src->lock: a sibling thread sharing src (THREAD_SHARE_FILES) can be in
// sys_close()/fd_table_grow() concurrently, and the old lock-free copy read a
// slot then vfs_dup'd it after the sibling's vfs_close had already freed the
// file -> a use-after-free (vfs_dup loads f->refcount on freed memory).  Holding
// src->lock makes a non-NULL slot guaranteed-live (its own ref keeps refcount
// >= 1, and close cannot NULL+free it without the lock), so vfs_dup is safe; the
// lock also pins src->ft against a concurrent grow's RCU free.  vfs_dup / the
// fdtable_alloc kmalloc never sleep, so holding the spinlock across them is sound
// (fd_table_grow already allocates under this same lock).  Returns 1, or 0 on
// OOM (dst->ft stays NULL, which task_files_release handles).
uint8_t fd_table_clone(task_files_t* dst, task_files_t* src) {
    spin_lock(&src->lock);
    fdtable_t* sft = src->ft;
    if (!fd_table_init(dst, sft->cap)) { spin_unlock(&src->lock); return 0; }
    fdtable_t* dft = dst->ft;
    for (uint32_t i = 0; i < sft->cap; i++) {
        dft->fd_table[i] = vfs_dup(sft->fd_table[i]);
        dft->fd_flags[i] = sft->fd_flags[i];
    }
    spin_unlock(&src->lock);
    return 1;
}

// Deterministic correctness guard for fd_table_clone (F126): clone a small
// table and assert cap + each slot pointer + flags are copied and every cloned
// file's refcount is bumped (the per-fd new reference).  The race fix itself
// (the src->lock around the copy) is code-proven + boot-exercised -- every
// fork/spawn/thread-create clones through here -- so this pins the copy logic.
void fd_table_clone_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    static vfs_file_t fa, fb;
    __builtin_memset(&fa, 0, sizeof fa); fa.refcount = 1;
    __builtin_memset(&fb, 0, sizeof fb); fb.refcount = 1;

    task_files_t src; __builtin_memset(&src, 0, sizeof src);
    spin_lock_init(&src.lock); src.refs = 1;
    task_files_t dst; __builtin_memset(&dst, 0, sizeof dst);
    spin_lock_init(&dst.lock); dst.refs = 1;

    if (!fd_table_init(&src, 8)) { kprintf("[fd_clone] FAIL src init\n"); return; }
    src.ft->fd_table[0] = &fa; src.ft->fd_flags[0] = 7u;
    src.ft->fd_table[3] = &fb;

    if (!fd_table_clone(&dst, &src)) { kprintf("[fd_clone] FAIL clone\n"); fails++; }
    else {
        if (dst.ft->cap != 8) fails++;
        if (dst.ft->fd_table[0] != &fa || dst.ft->fd_table[3] != &fb) fails++;  // same desc shared
        if (dst.ft->fd_flags[0] != 7u) fails++;                                 // FD_CLOEXEC preserved
        if (dst.ft->fd_table[1] != NULL || dst.ft->fd_table[2] != NULL) fails++;// empty slots stay NULL
        if (fa.refcount != 2u || fb.refcount != 2u) fails++;                    // each got a new ref
    }

    if (dst.ft) { kfree(dst.ft->fd_table); kfree(dst.ft->fd_flags); kfree(dst.ft); }
    kfree(src.ft->fd_table); kfree(src.ft->fd_flags); kfree(src.ft);
    kprintf(fails ? "[fd_clone] SELF-TEST FAILED\n"
                  : "[fd_clone] SELF-TEST PASSED (cap+slots+flags copied, refs bumped)\n");
}

task_files_t* task_files_alloc(void) {
    task_files_t* f = kmalloc(sizeof(task_files_t));
    if (!f) return NULL;
    f->ft   = NULL;
    f->refs = 1;
    spin_lock_init(&f->lock);
    return f;
}

// RCU callback: free the fd-table struct memory after a grace period.  A
// concurrent /proc/<pid>/fd reader snapshots the target's task_files_t + its
// fdtable_t under rcu_read_lock; deferring the free keeps that snapshot valid
// (the fds themselves are already closed synchronously in task_files_release).
static void task_files_free_rcu(void* data) {
    task_files_t* f = (task_files_t*)data;
    fdtable_t* ft = f->ft;
    if (ft) {
        kfree(ft->fd_table);
        kfree(ft->fd_flags);
        kfree(ft);
    }
    kfree(f);
}

void task_files_release(task_files_t* f) {
    if (!f) return;
    // ATOMIC — see task_mm_release.  Shared fd tables are inc/dec'd
    // concurrently by sharing threads; a torn RMW frees the table early.
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    fdtable_t* ft = f->ft;
    if (ft) {
        // Close the fds NOW (synchronous) so pipe/socket/tty peers see EOF at
        // exit time -- the load-bearing exit_files() semantics.
        for (uint32_t i = 0; i < ft->cap; i++)
            if (ft->fd_table[i]) vfs_close(ft->fd_table[i]);
    }
    // ...but DEFER the struct free: a /proc/<pid>/fd reader may be mid-snapshot
    // of this table under rcu_read_lock.  sys_exit unpublishes files_shared
    // BEFORE this release so no new reader can grab it; the grace period then
    // waits out any reader that already did, closing the cross-process UAF.
    call_rcu_expedited(task_files_free_rcu, f);
}

// PRIMITIVE: drop a task's fd table on exit, in the ONE correct order.
// task_files_release RCU-defers the table-struct free (a /proc/<pid>/fd reader
// snapshots files_shared, then tf->ft, then a fd, under rcu_read_lock), and
// call_rcu_expedited runs the grace period + frees INLINE.  So the caller MUST
// unpublish files_shared with a RELEASE store BEFORE the release: otherwise the
// inline grace period elapses while the pointer is still published, and a reader
// that opens its rcu_read_lock section just after it loads an already-freed
// table -> cross-process UAF (freed task_files_t / fdtable_t, and vfs_tryget on
// a freed vfs_file_t vtable).  Both exit paths (sys_exit and the fatal-signal
// SIG_DFL terminate) go through here so the unpublish-before-release order is a
// single source of truth and cannot drift -- it previously HAD drifted: the
// signal path freed first and NULLed after (a plain store), reopening the UAF on
// every fatal-signal exit whose fd table hit refcount 0 racing ps / readdir of
// /proc/<pid>/fd.
void task_drop_files(task_t* t) {
    task_files_t* tf = t->files_shared;
    if (!tf) return;
    __atomic_store_n(&t->files_shared, NULL, __ATOMIC_RELEASE);
    task_files_release(tf);
}

// ── PID pool ──────────────────────────────────────────────────────────────
// Bitmap allocation: O(1) amortised via next-fit cursor + ctzll on free words.
// 65535 PIDs = 1024 x 64-bit words = 8 KiB BSS.
// PIDs 0-9 reserved (kernel); userland starts at 10.
#define PID_MAX   65535u
#define PID_WORDS ((PID_MAX + 1 + 63u) / 64u)   // 1024 words

static uint64_t s_pid_bitmap[PID_WORDS] = { [0] = 0x3FFu }; // reserve 0-9
static uint32_t s_pid_next = 10;
// The pid pool is mutated lock-free from every task/thread/kthread
// spawner across all CPUs.  A non-atomic bitmap RMW let two concurrent
// pid_alloc()s pick the SAME free bit and hand out a DUPLICATE pid —
// pid_ht then aliases two tasks, and lookups/signals hit the wrong one.
// One serialized path; the critical section is a tiny O(1)-amortized scan.
static spinlock_t s_pid_lock;

uint32_t pid_alloc(void) {
    uint64_t _f = spin_lock_irqsave(&s_pid_lock);
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
            spin_unlock_irqrestore(&s_pid_lock, _f);
            return pid;
        }
    }
    spin_unlock_irqrestore(&s_pid_lock, _f);
    return 0; // out of PIDs
}

void pid_free(uint32_t pid) {
    if (pid < 10u || pid > PID_MAX) return;
    uint64_t _f = spin_lock_irqsave(&s_pid_lock);
    s_pid_bitmap[pid / 64u] &= ~(1ull << (pid % 64u));
    spin_unlock_irqrestore(&s_pid_lock, _f);
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
    t->cleartid_addr     = 0;
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

// PRIMITIVE (task-create unwind).  Single source of truth for the "allocated a
// PML4 frame (+ maybe an mm_t), a later step failed" cleanup shared by
// task_create_kthread / task_create_user / task_fork -- drop the mm_t if any,
// free the user page tables + the PML4 frame, then free the task_t.  Hand-rolling
// this sequence is exactly how two create paths used to LEAK the PML4 frame + mm.
// `pml4` MUST be a valid frame (the PMM_INVALID_ADDR alloc-failure case is handled
// by the caller before any mm exists, with a bare kfree(t)); `mm == NULL` is valid
// (kernel threads, and the failure points reached before an mm is built).
// Tear down a half-built task.  destroy_mm (if any) is the child's OWN mm to
// free.  layout_mm describes pml4's VMA layout so the leaf-frame free can skip
// VMA_MMIO frames: vmm_clone_user_ex shares a VMA_MMIO frame into a forked
// child WITHOUT a per-PTE pmm ref (the frame's lifetime is owned by drm.c), so
// an unconditional pmm_ref_dec here would drop the PARENT's still-live, GPU-
// scanned device frame -> UAF.  For a fork, layout_mm is the parent's mm (same
// VMA layout); for an empty/kernel-only pml4 it is NULL (no MMIO to skip).
// Free the frames BEFORE mm_destroy, which frees the VMAs vmm_free_user_ex
// consults.
static void task_create_unwind(task_t* t, phys_addr_t pml4, mm_t* destroy_mm,
                               mm_t* layout_mm) {
    vmm_free_user_ex(pml4, layout_mm);
    if (destroy_mm) mm_destroy(destroy_mm);
    pmm_buddy_free(pml4, 0);
    kfree(t);
}

// ── task_create_kthread ───────────────────────────────────────────────────
task_t* task_create_kthread(void (*entry)(void), uint32_t pid) {
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return NULL;
    // Drift-proof: kmalloc returns slab poison.  Zero the whole task_t so any
    // member not explicitly initialized below defaults to 0 instead of garbage
    // (see the exec paths' history of init drift); explicit inits override.
    __builtin_memset(t, 0, sizeof(*t));

    // Capture the PML4 so it can be freed on the task_mm_alloc failure below
    // (it used to be passed inline to task_mm_alloc and leaked on failure).
    phys_addr_t pml4 = vmm_alloc_pml4();
    if (pml4 == PMM_INVALID_ADDR) { kfree(t); return NULL; }
    task_mm_t* mm = task_mm_alloc(pml4, NULL);
    if (!mm) { task_create_unwind(t, pml4, NULL, NULL); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(mm); kfree(t); return NULL; }
    // fd_table_init OOM leaves files->ft == NULL -> a later fdget/fd_install
    // NULL-derefs.  Release and fail rather than build a task with no fd table.
    if (!fd_table_init(files, FD_INITIAL_CAP)) {
        task_files_release(files); task_mm_release(mm); kfree(t); return NULL;
    }
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
    // Drift-proof: kmalloc returns slab poison.  Zero the whole task_t so any
    // member not explicitly initialized below defaults to 0 instead of garbage
    // (see the exec paths' history of init drift); explicit inits override.
    __builtin_memset(t, 0, sizeof(*t));

    phys_addr_t pml4 = vmm_alloc_pml4();
    if (pml4 == PMM_INVALID_ADDR) { kfree(t); return NULL; }
    mm_t* mm = mm_create();
    // mm_create() returns NULL on OOM; task_mm_alloc(pml4, NULL) would SUCCEED
    // and the mm_vma_add(mm, ...) below would then NULL-deref.  Fail here, freeing
    // the PML4 frame (which used to be leaked along with the mm on task_mm_alloc
    // failure too).
    if (!mm) { task_create_unwind(t, pml4, NULL, NULL); return NULL; }
    task_mm_t* tmm = task_mm_alloc(pml4, mm);
    if (!tmm) { task_create_unwind(t, pml4, mm, NULL); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(tmm); kfree(t); return NULL; }
    // fd_table_init OOM leaves files->ft == NULL -> a later fdget/fd_install
    // NULL-derefs.  Release and fail rather than build a task with no fd table.
    if (!fd_table_init(files, FD_INITIAL_CAP)) {
        task_files_release(files); task_mm_release(tmm); kfree(t); return NULL;
    }
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

task_mm_t* task_get_mm_shared(void* task) {
    return task ? ((task_t*)task)->mm_shared : NULL;
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
    // Disown any signalfd that still references this task (an inherited or
    // SCM_RIGHTS-passed signalfd outlives its owner): NULL its raw owner
    // pointer before we free the task_t, or a later read/poll/close on the
    // surviving fd would dereference freed memory.  Must precede kfree(t).
    extern void signalfd_disown_all(task_t* t);
    signalfd_disown_all(t);
    kfree(t);
}

// Does this task self-reap as TASK_DEAD (vs become a wait()-able zombie) on
// exit?  A user thread (TASK_FLAG_THREAD) is joined via the CLEARTID futex, not
// wait()ed, so it MUST NOT linger as a zombie -- nobody reaps it, leaking its
// task-struct + pid forever.  A process leader (no THREAD flag) becomes a
// zombie its parent collects with waitpid().  Pure: the single decision both
// exit paths consult.
int task_exit_self_reaps(uint32_t flags) {
    return (flags & TASK_FLAG_THREAD) != 0;
}

// The ONE exit-disposition mechanism, shared by sys_exit and the fatal-signal
// terminate path so the two cannot drift.  (The bug this unifies: the signal
// path zombified a TASK_FLAG_THREAD exactly like a leader, leaking the dead
// thread's task-struct + pid and spuriously SIGCHLD-ing the process parent --
// sys_exit had the thread check, the signal path did not.)
//
//   - A user thread self-reaps as TASK_DEAD: the idle reaper collects it the
//     moment we switch away (do_switch) and releases the shared mm/files via
//     task_free_rcu.  No zombie linger, no SIGCHLD to the process parent (which
//     would spuriously wake its child reaper).
//   - A process leader becomes TASK_ZOMBIE its parent can waitpid(), and
//     SIGCHLD wakes that parent.
//
// The caller owns the surrounding teardown (cleartid, child reparent, files
// release) and chooses exit_code's meaning (normal code vs -signo).
void task_set_exit_state(task_t* t, int32_t exit_code) {
    t->exit_code = exit_code;
    if (task_exit_self_reaps(t->flags)) {
        t->state = TASK_DEAD;            // idle reaper frees it
    } else {
        t->state = TASK_ZOMBIE;
        sched_add_zombie(t);
        signal_send_pid(t->ppid, SIGCHLD);
    }
}

// Deterministic selftest for the exit-disposition mechanism (the F66 fix).
void task_exit_state_selftest(void) {
    extern void kprintf_atomic(const char*, ...);
    int fails = 0;

    // 1. The pure decision, every flag combination.
    if (task_exit_self_reaps(TASK_FLAG_THREAD) != 1) { fails++;
        kprintf_atomic("[task_exit_state] FAIL thread !self-reap\n"); }
    if (task_exit_self_reaps(0) != 0) { fails++;
        kprintf_atomic("[task_exit_state] FAIL leader self-reaps\n"); }
    if (task_exit_self_reaps(TASK_FLAG_KTHREAD) != 0) { fails++;
        kprintf_atomic("[task_exit_state] FAIL kthread self-reaps\n"); }
    if (task_exit_self_reaps(TASK_FLAG_THREAD | TASK_FLAG_KTHREAD) != 1) { fails++;
        kprintf_atomic("[task_exit_state] FAIL thread+kthread !self-reap\n"); }

    // 2. Integration: a THREAD task self-reaps as TASK_DEAD with no side
    //    effects (the FIXED path -- previously it was zombified and leaked).
    //    The task is never scheduled/listed, so no reaper observes it; the
    //    thread branch makes no list/signal calls, so a plain kfree is safe.
    task_t* th = kmalloc(sizeof(task_t));
    if (th) {
        __builtin_memset(th, 0, sizeof(*th));
        th->flags = TASK_FLAG_THREAD;
        th->state = TASK_RUNNING;
        th->ppid  = 0;
        task_set_exit_state(th, -11 /* -SIGSEGV */);
        if (th->state != TASK_DEAD || th->exit_code != -11) { fails++;
            kprintf_atomic("[task_exit_state] FAIL thread disposition state=%u code=%d\n",
                           (unsigned)th->state, (int)th->exit_code); }
        kfree(th);
    } else { fails++; kprintf_atomic("[task_exit_state] FAIL alloc\n"); }

    kprintf_atomic(fails ? "[task_exit_state] SELF-TEST FAILED\n"
                         : "[task_exit_state] PASS (thread self-reaps TASK_DEAD, leader zombifies)\n");
}

// Deterministic selftest for task_drop_files: the unpublish-BEFORE-release
// ordering that closes the /proc/<pid>/fd-reader-vs-RCU-deferred-free UAF.
// Uses a table with refs==2 so the release inside task_drop_files only
// DECREMENTS (no inline grace period, no fd-close of a fake table) -- proving
// the caller's files_shared is NULLed AND the table is released exactly once.
// A revert to release-before-unpublish would NOT fail this (the order is
// invisible single-threaded), but a revert that DROPS the unpublish entirely
// (leaving files_shared dangling) would -- and the code-proof + the heavy
// boot exercise (every process exit routes through task_drop_files) cover the
// cross-CPU race itself.
void task_files_drop_selftest(void) {
    extern void kprintf_atomic(const char*, ...);
    int fails = 0;

    task_files_t* f = task_files_alloc();   // refs = 1, ft = NULL
    task_t* t = kmalloc(sizeof(task_t));
    if (f && t) {
        f->refs = 2;                         // a second sharer keeps it alive past one release
        __builtin_memset(t, 0, sizeof(*t));
        t->files_shared = f;

        task_drop_files(t);                  // must: NULL t->files_shared, then release (2 -> 1)

        if (t->files_shared != NULL) { fails++;
            kprintf_atomic("[task_drop_files] FAIL files_shared not unpublished\n"); }
        if (f->refs != 1) { fails++;
            kprintf_atomic("[task_drop_files] FAIL refs=%u expected 1\n", (unsigned)f->refs); }

        task_files_release(f);               // drop the last ref (1 -> 0, RCU-deferred free)
    } else {
        fails++;
        kprintf_atomic("[task_drop_files] FAIL alloc\n");
        if (f) task_files_release(f);
    }
    if (t) kfree(t);

    kprintf_atomic(fails ? "[task_drop_files] SELF-TEST FAILED\n"
                         : "[task_drop_files] PASS (unpublish-before-release, table released once)\n");
}

void task_destroy(task_t* t) {
    if (!t) return;
    // Safety net: children are normally drained onto init at exit, so a task
    // being reaped has an empty children list and this is a no-op (atomic XCHG
    // of NULL).  But with children anchored on the thread-group leader, a
    // surviving thread could fork onto an already-exited leader in the window
    // before that leader is reaped; reparent any such stragglers to init rather
    // than leak them.  Skip if t IS init (reparent-to-self is a no-op anyway).
    if (g_init_task && g_init_task != t)
        task_children_reparent(t, g_init_task);

    // Remove from pid_ht first.  Zombies STAY in pid_ht until this
    // final destroy, so every specific-pid lookup is O(1) for
    // zombies AND living tasks.
    pid_ht_remove(t);
    pid_free(t->pid);

    // Unlink from the pgid/tgid/sid signal-routing tables BEFORE the
    // RCU-deferred free, so signal_send_pgrp / signal_send_group can
    // never walk a freed node.  sched_add_zombie also calls this at
    // zombie time (a prompt-removal optimization), but the pthread
    // exit path (TASK_FLAG_THREAD -> TASK_DEAD, reaped here without
    // ever becoming a zombie) skips sched_add_zombie entirely, so
    // task_idx_remove MUST run at this universal final-destroy
    // chokepoint or the dead thread stays linked in those lists with
    // its storage freed underneath it.  task_idx_remove is idempotent
    // (a node already unlinked has NULL prev/next and is not the bucket
    // head, so the second call is a no-op), so the zombie path that
    // removed early is unaffected.  Lock order is safe: task_destroy
    // holds no rq_lock at either call site (the do_switch reaper
    // releases rq_lock before context_switch; sys_wait reaps after
    // sched_reap_zombie has released it), and the table lock sorts
    // ABOVE rq_lock, so taking it here cannot invert.
    task_idx_remove(t);

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
    // Drift-proof: kmalloc returns slab poison.  Zero the whole task_t so any
    // member not explicitly initialized below defaults to 0 instead of garbage
    // (see the exec paths' history of init drift); explicit inits override.
    __builtin_memset(t, 0, sizeof(*t));

    // Fork = new process: deep-copy both mm and files.
    phys_addr_t new_pml4 = vmm_alloc_pml4();
    if (new_pml4 == PMM_INVALID_ADDR) { kfree(t); return NULL; }

    if (!vmm_clone_user_ex(new_pml4, parent->mm_shared->pml4_phys,
                           parent->mm_shared->mm)) {
        task_create_unwind(t, new_pml4, NULL, parent->mm_shared->mm);
        return NULL;
    }

    // CoW marking just write-protected every parent PTE — but only
    // the local CPU's TLB got flushed (inside the clone).  Any OTHER
    // CPU running a parent thread still holds stale WRITABLE entries
    // and keeps writing into frames that CoW break-off has already
    // redirected — silent heap corruption in every multithreaded
    // parent that forks (foot's font loaders died of this; sway's
    // glib worker too).  Shoot down every CPU running this mm.
    {
        extern void tlb_flush_mm(task_mm_t* mm);
        tlb_flush_mm(parent->mm_shared);
    }

    mm_t* new_mm = mm_clone(parent->mm_shared->mm);
    if (!new_mm) {
        task_create_unwind(t, new_pml4, NULL, parent->mm_shared->mm);
        return NULL;
    }

    task_mm_t* tmm = task_mm_alloc(new_pml4, new_mm);
    if (!tmm) {
        task_create_unwind(t, new_pml4, new_mm, parent->mm_shared->mm);
        return NULL;
    }

    task_files_t* files = task_files_alloc();
    if (!files) { task_mm_release(tmm); kfree(t); return NULL; }
    // Copy the parent's fd table under the parent's lock: a sibling thread
    // sharing the table can close()/grow() concurrently, so the old lock-free
    // vfs_dup loop could vfs_dup a file a sibling had just freed (a UAF).
    if (!fd_table_clone(files, parent->files_shared)) {
        task_files_release(files); task_mm_release(tmm); kfree(t); return NULL;
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
    t->cleartid_addr     = 0;    // join handles belong to the original thread
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
