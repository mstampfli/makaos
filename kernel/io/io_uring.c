// ── io_uring instance lifecycle, ring mapping, CQE posting (Phase 8A/B/C) ─
//
// Instance memory is a single contiguous physical block, mapped into
// the creating task's user address space via vmm_map_physical_user.
// The kernel keeps HHDM-aliased pointers to the same memory so it
// can read/write ring state without going through the user mapping
// (faster and doesn't touch user TLB).
//
// Layout (all power-of-two sized):
//   offset 0                     :  io_sq_ring_hdr_t
//   offset sizeof(sq_hdr)        :  io_cq_ring_hdr_t
//   PAGE_SIZE                    :  sqes[sq_entries]     (64 B each)
//   next page aligned            :  cqes[cq_entries]     (16 B each)
//
// 256 entries → 256*64 SQE + 512*16 CQE + headers = ~25 KB → 7 pages
// (rounded up to order-3 buddy block = 8 pages).

#include "io_uring.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "common.h"
#include "process.h"
#include "sched.h"
#include "vfs.h"
#include "wait.h"
#include "rcu.h"
#include "errno.h"
#include "kheap.h"

extern task_t* g_current_bypass;      // unused here; kept to silence linter
extern uint64_t tsc_read_ns(void);

#define IO_URING_MIN_ENTRIES 1u
#define IO_URING_MAX_ENTRIES 4096u

// Forward declarations for helpers used across phases.
static int     io_sqp_spawn(io_uring_t* uring);
static int     io_wq_enqueue(io_uring_t* uring, const io_sqe_t* sqe);
static int     io_wq_enqueue_chain(io_uring_t* uring, uint32_t* phead,
                                     uint32_t user_tail, uint32_t mask);
static int32_t dispatch_exec(io_uring_t* uring, const io_sqe_t* sqe);

// Round up to next power of two, minimum 1.
static uint32_t round_pow2_u32(uint32_t v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// Compute total backing bytes for given entries.  Page-aligned.
static uint64_t compute_layout(uint32_t sq_entries, uint32_t cq_entries,
                                uint64_t* out_sqes_off,
                                uint64_t* out_cqes_off) {
    uint64_t hdrs_end = (uint64_t)sizeof(io_sq_ring_hdr_t)
                      + (uint64_t)sizeof(io_cq_ring_hdr_t);
    // SQEs start at the next page boundary so user mmap semantics work
    // even though we use vmm_map_physical_user (no offset mmap).
    uint64_t sqes_off = (hdrs_end + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
    uint64_t sqes_bytes = (uint64_t)sq_entries * sizeof(io_sqe_t);
    uint64_t cqes_off = (sqes_off + sqes_bytes + PAGE_SIZE - 1)
                        & ~(uint64_t)PAGE_MASK;
    uint64_t cqes_bytes = (uint64_t)cq_entries * sizeof(io_cqe_t);
    uint64_t total = (cqes_off + cqes_bytes + PAGE_SIZE - 1)
                     & ~(uint64_t)PAGE_MASK;

    *out_sqes_off = sqes_off;
    *out_cqes_off = cqes_off;
    return total;
}

// vfs_file_t ops — a ring fd is a pure handle.  Read/write are not
// supported; close releases the backing pages.  The dispatcher uses
// f->ctx = io_uring_t*.
static int64_t ring_read_stub (struct vfs_file_t* f, void* b, uint64_t n) {
    (void)f; (void)b; (void)n; return -1;
}
static int64_t ring_write_stub(struct vfs_file_t* f, const void* b, uint64_t n) {
    (void)f; (void)b; (void)n; return -1;
}

void io_uring_close_file(struct vfs_file_t* f) {
    if (!f) return;
    io_uring_t* uring = (io_uring_t*)f->ctx;
    if (uring) {
        // Phase 8E: stop the worker.  Set worker_stop then wake so
        // the worker exits its wait loop and transitions to TASK_DEAD.
        // We do NOT wait for the worker here — it'll reap itself via
        // the scheduler's normal zombie path.
        if (uring->worker) {
            __atomic_store_n(&uring->worker_stop, 1u, __ATOMIC_RELEASE);
            wait_queue_wake_all(&uring->wq_waitq);
        }
        // Phase 8F: stop the SQPOLL kthread similarly.
        if (uring->sqp_task) {
            __atomic_store_n(&uring->sqp_stop, 1u, __ATOMIC_RELEASE);
            wait_queue_wake_all(&uring->sqp_waitq);
        }
        // Phase 8G: release fixed files.
        if (uring->fixed_files) {
            for (uint32_t i = 0; i < uring->fixed_files_nr; i++)
                if (uring->fixed_files[i]) vfs_close(uring->fixed_files[i]);
            kfree(uring->fixed_files);
            uring->fixed_files = NULL;
        }
        // Free backing pages.  The user mapping will have been torn
        // down already by task exit (task_mm_release walks all VMAs).
        // We only own the phys pages here.
        uint8_t order = 0;
        uint64_t pages = uring->backing_bytes >> PAGE_SHIFT;
        while ((1ULL << order) < pages) order++;
        pmm_buddy_free(uring->backing_phys, order);
        kfree(uring);
    }
    kfree(f);
}

struct vfs_file_t* io_uring_create(uint32_t entries,
                                    io_uring_params_t* kparams) {
    if (!kparams) return NULL;
    if (entries == 0) return NULL;
    if (entries > IO_URING_MAX_ENTRIES) entries = IO_URING_MAX_ENTRIES;

    uint32_t sq_entries = round_pow2_u32(entries);
    uint32_t cq_entries = sq_entries * 2;   // Linux's default

    uint64_t sqes_off, cqes_off;
    uint64_t total_bytes = compute_layout(sq_entries, cq_entries,
                                            &sqes_off, &cqes_off);

    // Alloc a power-of-two buddy block covering total_bytes.
    uint64_t total_pages = total_bytes >> PAGE_SHIFT;
    uint8_t  order = 0;
    while ((1ULL << order) < total_pages) order++;

    phys_addr_t phys = pmm_buddy_alloc(order);
    if (phys == PMM_INVALID_ADDR) return NULL;

    // Zero the backing (important — ring headers must start {head=0, tail=0}).
    __builtin_memset((void*)(phys + HHDM_OFFSET), 0, total_bytes);

    // Allocate instance struct.
    io_uring_t* uring = (io_uring_t*)kmalloc(sizeof(*uring));
    if (!uring) { pmm_buddy_free(phys, order); return NULL; }
    __builtin_memset(uring, 0, sizeof(*uring));

    uring->backing_phys = phys;
    uring->backing_bytes = total_bytes;
    uring->sq_entries = sq_entries;
    uring->cq_entries = cq_entries;
    uring->flags = kparams->flags;
    uring->owner_mm   = g_current->mm_shared ? g_current->mm_shared->mm : NULL;
    uring->owner_task = g_current;
    spin_lock_init(&uring->cq_lock);
    wait_queue_init(&uring->waitq);
    wait_queue_init(&uring->wq_waitq);
    wait_queue_init(&uring->sqp_waitq);
    uring->worker = NULL;
    uring->wq_head = NULL;
    uring->worker_stop = 0;
    uring->sqp_task = NULL;
    uring->sqp_stop = 0;
    uring->sqp_idle_ms = kparams->sq_thread_idle ? kparams->sq_thread_idle : 1000;
    uring->fixed_files    = NULL;
    uring->fixed_files_nr = 0;
    spin_lock_init(&uring->fixed_files_lock);
    uring->overflow_head = NULL;
    uring->overflow_tail = NULL;
    spin_lock_init(&uring->overflow_lock);

    // Kernel-side HHDM aliases — read/write these directly, no fault.
    uint8_t* kbase = (uint8_t*)(phys + HHDM_OFFSET);
    uring->sq_hdr = (io_sq_ring_hdr_t*)(kbase);
    uring->cq_hdr = (io_cq_ring_hdr_t*)(kbase + sizeof(io_sq_ring_hdr_t));
    uring->sqes   = (io_sqe_t*)(kbase + sqes_off);
    uring->cqes   = (io_cqe_t*)(kbase + cqes_off);

    // Initialise ring metadata.
    uring->sq_hdr->ring_mask    = sq_entries - 1;
    uring->sq_hdr->ring_entries = sq_entries;
    uring->cq_hdr->ring_mask    = cq_entries - 1;
    uring->cq_hdr->ring_entries = cq_entries;

    // Map backing into user address space.  Read-write, user-visible,
    // non-executable.  The entire backing is one VMA.
    if (!uring->owner_mm) { kfree(uring); pmm_buddy_free(phys, order); return NULL; }
    virt_addr_t user_base = vmm_map_physical_user(
        uring->owner_mm,
        g_current->mm_shared->pml4_phys,
        phys, total_bytes);
    if (!user_base) {
        kfree(uring);
        pmm_buddy_free(phys, order);
        return NULL;
    }
    uring->user_vaddr = user_base;

    // Fill out params for the caller.
    kparams->sq_entries  = sq_entries;
    kparams->cq_entries  = cq_entries;
    kparams->features    = 0;
    kparams->sq_ring_ptr = (uint64_t)user_base + 0;
    kparams->cq_ring_ptr = (uint64_t)user_base + sizeof(io_sq_ring_hdr_t);
    kparams->sqes_ptr    = (uint64_t)user_base + sqes_off;
    kparams->cqes_ptr    = (uint64_t)user_base + cqes_off;

    // Allocate vfs_file_t for fd installation.  Uses kheap so the
    // caller can later fd_install it.
    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) {
        // Teardown: user mapping, backing, uring.  The user VMA is
        // leaked for now (mm cleanup on task exit will free it).
        kfree(uring);
        pmm_buddy_free(phys, order);
        return NULL;
    }
    __builtin_memset(f, 0, sizeof(*f));
    f->read  = ring_read_stub;
    f->write = ring_write_stub;
    f->close = io_uring_close_file;
    f->ctx   = uring;
    f->waitq = &f->_waitq;
    wait_queue_init(&f->_waitq);
    f->refcount = 1;
    f->rights   = 0xFFFFFFFFu;

    // Phase 8F: spawn SQPOLL kthread if requested.  The poller shares
    // the owner's mm + files so it can touch user buffers and fds.
    if (kparams->flags & IORING_SETUP_SQPOLL) {
        int r = io_sqp_spawn(uring);
        if (r < 0) {
            // Spawn failure is fatal for SQPOLL rings.  Rollback the
            // whole setup.
            io_uring_close_file(f);
            return NULL;
        }
    }

    return f;
}

// Post a CQE at the tail.  cq_lock serialises concurrent posters
// (multiple io_wq workers in 8E).
//
// Overflow handling: if the CQ is full, the CQE is stashed on a
// kernel-side overflow list instead of being dropped.  The next
// post_cqe call that finds CQ space drains the overflow list
// head-first BEFORE publishing its own CQE — guaranteeing FIFO
// delivery.  Matches Linux io_uring's overflow semantics.
//
// Drain-on-post is cheap: one atomic_load of the head, one
// kmalloc'd node free per drained entry, and the overflow list
// is short under normal load (only fills during burst overflow).
void io_uring_post_cqe(io_uring_t* uring, uint64_t user_data,
                        int32_t res, uint32_t cqe_flags) {
    uint64_t f = spin_lock_irqsave(&uring->cq_lock);
    uint32_t mask = uring->cq_hdr->ring_mask;

    // Drain any pending overflow entries that now fit.
    if (uring->overflow_head) {
        uint64_t of = spin_lock_irqsave(&uring->overflow_lock);
        while (uring->overflow_head) {
            uint32_t tail = uring->cq_hdr->tail;
            uint32_t head = __atomic_load_n(&uring->cq_hdr->head,
                                              __ATOMIC_ACQUIRE);
            if ((tail - head) >= uring->cq_entries) break;

            struct io_overflow_cqe* n = uring->overflow_head;
            io_cqe_t* slot = &uring->cqes[tail & mask];
            *slot = n->cqe;
            __atomic_store_n(&uring->cq_hdr->tail, tail + 1,
                              __ATOMIC_RELEASE);

            uring->overflow_head = n->next;
            if (!uring->overflow_head) uring->overflow_tail = NULL;
            kfree(n);
        }
        spin_unlock_irqrestore(&uring->overflow_lock, of);
    }

    uint32_t tail = uring->cq_hdr->tail;
    uint32_t head = __atomic_load_n(&uring->cq_hdr->head, __ATOMIC_ACQUIRE);

    if ((tail - head) >= uring->cq_entries) {
        // CQ still full — push onto overflow list instead of
        // dropping.  Bump the user-visible overflow counter as a
        // hint (userspace can monitor it to throttle submissions).
        struct io_overflow_cqe* n = (struct io_overflow_cqe*)
            kmalloc(sizeof(*n));
        if (!n) {
            // OOM during overflow — last resort: drop and count.
            uring->cq_hdr->overflow++;
            spin_unlock_irqrestore(&uring->cq_lock, f);
            wait_queue_wake_all(&uring->waitq);
            return;
        }
        n->next          = NULL;
        n->cqe.user_data = user_data;
        n->cqe.res       = res;
        n->cqe.flags     = cqe_flags;

        uint64_t of = spin_lock_irqsave(&uring->overflow_lock);
        if (uring->overflow_tail) uring->overflow_tail->next = n;
        else                      uring->overflow_head = n;
        uring->overflow_tail = n;
        spin_unlock_irqrestore(&uring->overflow_lock, of);

        uring->cq_hdr->overflow++;
        spin_unlock_irqrestore(&uring->cq_lock, f);
        wait_queue_wake_all(&uring->waitq);
        return;
    }

    io_cqe_t* slot = &uring->cqes[tail & mask];
    slot->user_data = user_data;
    slot->res       = res;
    slot->flags     = cqe_flags;

    __atomic_store_n(&uring->cq_hdr->tail, tail + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&uring->cq_lock, f);

    wait_queue_wake_all(&uring->waitq);
}

// ── Phase 8C: synchronous SQ processor ───────────────────────────────
// Walks SQEs from kernel-tracked sq_head to user-written sq_tail.
// Dispatches each opcode → existing kernel impls.
//
// Executes in the caller's task context.  Blocking ops block the
// caller — Phase 8E offloads those to io_wq kthreads.

// Forward decls into existing syscall internals.  These are already
// available via the syscall dispatch; we just re-export prototypes
// so we don't accidentally re-implement.
extern int64_t vfs_read (struct vfs_file_t* f, void* buf, uint64_t len);
extern int64_t vfs_write(struct vfs_file_t* f, const void* buf, uint64_t len);

// fdget/fdput — mirror the syscall-layer helpers.  RCU-deref the
// published fdtable_t, bounds-check, tryget to race-safely bump the
// vfs_file_t refcount.
static vfs_file_t* uring_fdget(int32_t fd) {
    if (fd < 0) return NULL;
    task_t* t = g_current;
    if (!t || !t->files_shared) return NULL;
    task_files_t* tf = t->files_shared;
    rcu_read_lock();
    fdtable_t* ft = (fdtable_t*)rcu_dereference(tf->ft);
    if (!ft || (uint32_t)fd >= ft->cap) { rcu_read_unlock(); return NULL; }
    vfs_file_t* f = __atomic_load_n(&ft->fd_table[fd], __ATOMIC_ACQUIRE);
    vfs_file_t* got = vfs_tryget(f);
    rcu_read_unlock();
    return got;
}
static void uring_fdput(vfs_file_t* f) { vfs_close(f); }

// Non-static in syscall.c — canonical close semantics.
extern uint64_t sys_close(uint64_t fd);

// ── Phase 8G: fixed files ─────────────────────────────────────────
//
// Fast-path fd resolution: if IOSQE_FIXED_FILE is set, sqe->fd is an
// INDEX into uring->fixed_files[].  No atomic fd-table lookup, no
// refcount bump — the ring holds its own ref for the lifetime of
// the registration.
//
// Register takes a user-mode int[] of fds, looks up each via
// canonical fdget (bumps refcount), stores in the ring's array.
// Unregister vfs_closes each and frees the array.

extern int copy_from_user(void* dst, const void* src_u, uint64_t len);

int io_uring_register_impl(io_uring_t* uring, uint32_t op,
                            uint64_t arg_uptr, uint32_t nr_args) {
    if (!uring) return -EINVAL;
    uint64_t f = spin_lock_irqsave(&uring->fixed_files_lock);

    switch (op) {
        case IORING_REGISTER_FILES: {
            if (uring->fixed_files) { spin_unlock_irqrestore(&uring->fixed_files_lock, f); return -EBUSY; }
            if (nr_args == 0 || nr_args > 4096) { spin_unlock_irqrestore(&uring->fixed_files_lock, f); return -EINVAL; }

            // Allocate the kernel-side array.
            vfs_file_t** arr = (vfs_file_t**)kmalloc(nr_args * sizeof(vfs_file_t*));
            if (!arr) { spin_unlock_irqrestore(&uring->fixed_files_lock, f); return -ENOMEM; }
            __builtin_memset(arr, 0, nr_args * sizeof(vfs_file_t*));

            // Copy fds from user.  Need to release lock briefly for
            // copy_from_user since it can fault.  Another thread
            // concurrent-registering is blocked by the EBUSY we
            // already set... but we haven't set fixed_files yet.
            // Hold the lock across the copy — copy_from_user calls
            // are short memcpys with page-present checks, no sleep.
            int* fds = (int*)kmalloc(nr_args * sizeof(int));
            if (!fds) {
                kfree(arr);
                spin_unlock_irqrestore(&uring->fixed_files_lock, f);
                return -ENOMEM;
            }
            if (copy_from_user(fds, (const void*)arg_uptr,
                                 (uint64_t)nr_args * sizeof(int)) != 0) {
                kfree(fds); kfree(arr);
                spin_unlock_irqrestore(&uring->fixed_files_lock, f);
                return -EFAULT;
            }

            // Resolve each fd → vfs_file_t* + refcount bump.
            for (uint32_t i = 0; i < nr_args; i++) {
                int fd = fds[i];
                if (fd < 0) { arr[i] = NULL; continue; }   // -1 = sparse slot
                vfs_file_t* vf = uring_fdget(fd);
                if (!vf) {
                    // Rollback: drop refs on what we already took.
                    for (uint32_t j = 0; j < i; j++)
                        if (arr[j]) vfs_close(arr[j]);
                    kfree(fds); kfree(arr);
                    spin_unlock_irqrestore(&uring->fixed_files_lock, f);
                    return -EBADF;
                }
                arr[i] = vf;
            }

            kfree(fds);
            uring->fixed_files    = arr;
            uring->fixed_files_nr = nr_args;
            spin_unlock_irqrestore(&uring->fixed_files_lock, f);
            return 0;
        }
        case IORING_UNREGISTER_FILES: {
            if (!uring->fixed_files) { spin_unlock_irqrestore(&uring->fixed_files_lock, f); return -EINVAL; }
            vfs_file_t** arr = uring->fixed_files;
            uint32_t n = uring->fixed_files_nr;
            uring->fixed_files    = NULL;
            uring->fixed_files_nr = 0;
            spin_unlock_irqrestore(&uring->fixed_files_lock, f);

            // Release refs outside the lock — vfs_close may sleep.
            for (uint32_t i = 0; i < n; i++)
                if (arr[i]) vfs_close(arr[i]);
            kfree(arr);
            return 0;
        }
    }
    spin_unlock_irqrestore(&uring->fixed_files_lock, f);
    return -EINVAL;
}

// Look up an fd — either fixed-file index or regular fd, depending
// on the SQE flags.  Returns an owned reference (caller vfs_closes).
static vfs_file_t* sqe_fdget(io_uring_t* uring, const io_sqe_t* sqe) {
    if (sqe->flags & IOSQE_FIXED_FILE) {
        uint32_t idx = (uint32_t)sqe->fd;
        if (idx >= uring->fixed_files_nr) return NULL;
        vfs_file_t* f = uring->fixed_files[idx];
        return vfs_tryget(f);       // bump ref; fixed_files holds a
                                    // stable ref, so tryget always
                                    // succeeds unless under teardown
    }
    return uring_fdget(sqe->fd);
}

// ── Phase 8F: SQPOLL — kthread-only submission ──────────────────────
//
// When IORING_SETUP_SQPOLL is set, userspace can write SQEs and bump
// sq_tail WITHOUT calling io_uring_enter — a dedicated poller
// kthread watches sq_tail and dispatches on its own.
//
// Idle policy: after sq_thread_idle ms with no new work, the poller
// sets IORING_SQ_NEED_WAKEUP in sq_hdr->flags and parks on sqp_waitq.
// User sees the flag and must call io_uring_enter(ENTER_SQ_WAKEUP)
// to unpark.  Linux's batching heuristic exactly.

static void dispatch_one(io_uring_t* uring, const io_sqe_t* sqe);

static void io_sqp_kthread_entry(void) {
    io_uring_t* uring = (io_uring_t*)g_current->kthread_ctx;
    if (!uring) { g_current->state = TASK_DEAD; sched_yield(); return; }

    uint64_t idle_ns = (uint64_t)uring->sqp_idle_ms * 1000000ULL;
    uint64_t idle_deadline = 0;   // set when we first observe empty SQ

    for (;;) {
        if (__atomic_load_n(&uring->sqp_stop, __ATOMIC_ACQUIRE)) {
            g_current->state = TASK_DEAD;
            sched_yield();
            return;
        }

        uint32_t mask = uring->sq_hdr->ring_mask;
        uint32_t user_tail = __atomic_load_n(&uring->sq_hdr->tail, __ATOMIC_ACQUIRE);
        uint32_t head      = uring->sq_hdr->head;

        if (head != user_tail) {
            // Work available — drain it.
            idle_deadline = 0;   // reset idle timer
            // Clear NEED_WAKEUP before we start (user may set it again).
            uint32_t f = uring->sq_hdr->flags;
            if (f & IORING_SQ_NEED_WAKEUP)
                __atomic_fetch_and(&uring->sq_hdr->flags,
                                    ~IORING_SQ_NEED_WAKEUP,
                                    __ATOMIC_RELEASE);
            while (head != user_tail) {
                const io_sqe_t* sqe = &uring->sqes[head & mask];
                if (sqe->flags & IOSQE_ASYNC) {
                    io_wq_enqueue(uring, sqe);
                } else {
                    dispatch_one(uring, sqe);
                }
                head++;
            }
            __atomic_store_n(&uring->sq_hdr->head, head, __ATOMIC_RELEASE);
            continue;
        }

        // Empty SQ.  Spin briefly before parking — Linux SLUB-style
        // "busy for a bit in case a burst is coming".
        uint64_t now = tsc_read_ns();
        if (idle_deadline == 0) idle_deadline = now + idle_ns;
        if (now < idle_deadline) {
            // Short spin.  cpu_relax equivalent.
            __asm__ volatile("pause");
            continue;
        }

        // Park.  Publish NEED_WAKEUP so userspace knows to call
        // enter(ENTER_SQ_WAKEUP) before new submissions will be
        // seen.
        __atomic_fetch_or(&uring->sq_hdr->flags, IORING_SQ_NEED_WAKEUP,
                           __ATOMIC_RELEASE);
        // Re-check sq_tail to avoid lost wakeup: user may have
        // submitted between the tail read and the flag set.
        user_tail = __atomic_load_n(&uring->sq_hdr->tail, __ATOMIC_ACQUIRE);
        if (head != user_tail) {
            __atomic_fetch_and(&uring->sq_hdr->flags,
                                ~IORING_SQ_NEED_WAKEUP,
                                __ATOMIC_RELEASE);
            idle_deadline = 0;
            continue;
        }
        // Actually park.  Woken by enter(ENTER_SQ_WAKEUP) or sqp_stop.
        WAIT_EVENT(&uring->sqp_waitq, ({
            uint32_t ut = __atomic_load_n(&uring->sq_hdr->tail,
                                            __ATOMIC_ACQUIRE);
            uint32_t st = __atomic_load_n(&uring->sqp_stop,
                                            __ATOMIC_ACQUIRE);
            ut != uring->sq_hdr->head || st != 0;
        }));
        __atomic_fetch_and(&uring->sq_hdr->flags,
                            ~IORING_SQ_NEED_WAKEUP,
                            __ATOMIC_RELEASE);
        idle_deadline = 0;
    }
}

static int io_sqp_spawn(io_uring_t* uring) {
    if (uring->sqp_task) return 0;
    if (!uring->owner_task) return -EINVAL;

    task_t* t = task_create_kthread(io_sqp_kthread_entry, pid_alloc());
    if (!t) return -ENOMEM;

    if (t->mm_shared && t->mm_shared != uring->owner_task->mm_shared)
        task_mm_release(t->mm_shared);
    if (t->files_shared && t->files_shared != uring->owner_task->files_shared)
        task_files_release(t->files_shared);
    t->mm_shared    = uring->owner_task->mm_shared;
    t->files_shared = uring->owner_task->files_shared;
    if (t->mm_shared)    __atomic_add_fetch(&t->mm_shared->refs, 1, __ATOMIC_RELAXED);
    if (t->files_shared) __atomic_add_fetch(&t->files_shared->refs, 1, __ATOMIC_RELAXED);

    t->kthread_ctx = uring;
    uring->sqp_task = t;
    sched_add(t);
    return 0;
}

// ── Phase 8E: io_wq worker infrastructure ───────────────────────────
// A dedicated slab cache for io_wq_work_t so enqueue is one CAS +
// one slab alloc.  Worker drains via atomic_exchange (grabs the
// entire chain in one op), walks it, dispatches each, frees the
// work node.

static slab_cache_t s_wq_work_cache;
static uint8_t       s_wq_work_cache_inited = 0;

static io_wq_work_t* wq_work_alloc(void) {
    if (!s_wq_work_cache_inited) {
        pmm_slab_cache_init(&s_wq_work_cache, sizeof(io_wq_work_t), 0);
        s_wq_work_cache_inited = 1;
    }
    return (io_wq_work_t*)pmm_slab_alloc(&s_wq_work_cache);
}
static void wq_work_free(io_wq_work_t* w) { pmm_slab_free(w); }

static void dispatch_one(io_uring_t* uring, const io_sqe_t* sqe);

static void io_wq_worker_entry(void) {
    // Find our ring via a per-task handoff (see io_wq_spawn below).
    // We stash the ring pointer in task_t's steal_rng (unused for
    // kthreads that don't participate in work-stealing — good-enough
    // back-channel without a dedicated field).
    io_uring_t* uring = (io_uring_t*)g_current->kthread_ctx;
    if (!uring) { g_current->state = TASK_DEAD; sched_yield(); return; }

    for (;;) {
        // Drain queue with single atomic_exchange.
        io_wq_work_t* chain = (io_wq_work_t*)__atomic_exchange_n(
            &uring->wq_head, NULL, __ATOMIC_ACQUIRE);

        if (!chain) {
            if (__atomic_load_n(&uring->worker_stop, __ATOMIC_ACQUIRE)) {
                g_current->state = TASK_DEAD;
                sched_yield();
                return;
            }
            // Sleep until someone enqueues work OR worker_stop is set.
            WAIT_EVENT(&uring->wq_waitq, ({
                io_wq_work_t* h = (io_wq_work_t*)
                    __atomic_load_n(&uring->wq_head, __ATOMIC_ACQUIRE);
                uint32_t stop = __atomic_load_n(&uring->worker_stop,
                                                 __ATOMIC_ACQUIRE);
                h != NULL || stop != 0;
            }));
            continue;
        }

        // Walk the Treiber chain (LIFO between distinct submissions
        // is fine).  For each work item, walk its chain_next list
        // (linked chain within a single submission) sequentially —
        // cancel the chain tail on IO_LINK failure.
        while (chain) {
            io_wq_work_t* head_w = chain;
            chain = chain->next;

            // Walk one linked chain (one SQE or a sequence linked
            // via IOSQE_IO_LINK / IOSQE_IO_HARDLINK).
            io_wq_work_t* w = head_w;
            int cancelled = 0;
            while (w) {
                io_wq_work_t* nxt = w->chain_next;
                int32_t res;
                if (cancelled) {
                    res = -ECANCELED;
                } else {
                    res = dispatch_exec(uring, &w->sqe);
                }
                io_uring_post_cqe(uring, w->sqe.user_data, res, 0);
                // If this op had IO_LINK set and failed, cancel the
                // rest of the chain.  IO_HARDLINK ignores the failure
                // and keeps running.
                if (!cancelled && (w->sqe.flags & IOSQE_IO_LINK) && res < 0)
                    cancelled = 1;
                wq_work_free(w);
                w = nxt;
            }
        }
    }
}

// Spawn (or return existing) the worker for a ring.  Lazy — only
// created on first async-flagged op.  Shares mm_shared + files_shared
// with the ring's owner task so it can touch user buffers and fds.
static int io_wq_ensure_worker(io_uring_t* uring) {
    if (uring->worker) return 0;
    if (!uring->owner_task) return -EINVAL;

    task_t* t = task_create_kthread(io_wq_worker_entry, pid_alloc());
    if (!t) return -ENOMEM;

    // Swap the freshly-allocated kernel mm/files for the owner's.
    // refs++ keeps them alive for the worker's lifetime; release the
    // fresh ones the worker came with.
    if (t->mm_shared && t->mm_shared != uring->owner_task->mm_shared) {
        task_mm_release(t->mm_shared);
    }
    if (t->files_shared && t->files_shared != uring->owner_task->files_shared) {
        task_files_release(t->files_shared);
    }
    t->mm_shared    = uring->owner_task->mm_shared;
    t->files_shared = uring->owner_task->files_shared;
    if (t->mm_shared)    __atomic_add_fetch(&t->mm_shared->refs, 1, __ATOMIC_RELAXED);
    if (t->files_shared) __atomic_add_fetch(&t->files_shared->refs, 1, __ATOMIC_RELAXED);

    // Stash the ring pointer so the worker can find it.
    t->kthread_ctx = uring;
    uring->worker = t;
    sched_add(t);
    return 0;
}

// Enqueue a single SQE (no linked chain).  Single-CAS push.
static int io_wq_enqueue(io_uring_t* uring, const io_sqe_t* sqe) {
    int r = io_wq_ensure_worker(uring);
    if (r < 0) return r;

    io_wq_work_t* w = wq_work_alloc();
    if (!w) return -ENOMEM;
    w->sqe        = *sqe;
    w->uring      = uring;
    w->chain_next = NULL;

    io_wq_work_t* old = (io_wq_work_t*)__atomic_load_n(
        &uring->wq_head, __ATOMIC_RELAXED);
    do {
        w->next = old;
    } while (!__atomic_compare_exchange_n(&uring->wq_head, &old, w,
                                            0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
    wait_queue_wake_all(&uring->wq_waitq);
    return 0;
}

// Enqueue an SQE and any following IO_LINK/IO_HARDLINK SQEs as one
// chained work item.  *phead is advanced past the whole chain.
// The worker walks chain_next sequentially with IO_LINK cancel
// semantics (see worker loop).
static int io_wq_enqueue_chain(io_uring_t* uring, uint32_t* phead,
                                 uint32_t user_tail, uint32_t mask) {
    int r = io_wq_ensure_worker(uring);
    if (r < 0) return r;

    // Build the chain by following IO_LINK / IO_HARDLINK flags.
    io_wq_work_t* first = NULL;
    io_wq_work_t* tail  = NULL;

    for (;;) {
        if (*phead == user_tail) break;
        const io_sqe_t* sqe = &uring->sqes[*phead & mask];

        io_wq_work_t* w = wq_work_alloc();
        if (!w) {
            // Partial-failure cleanup: free what we've built.
            while (first) {
                io_wq_work_t* n = first->chain_next;
                wq_work_free(first);
                first = n;
            }
            return -ENOMEM;
        }
        w->sqe        = *sqe;
        w->uring      = uring;
        w->chain_next = NULL;
        if (!first) first = w;
        if (tail)   tail->chain_next = w;
        tail = w;

        uint8_t flags = sqe->flags;
        (*phead)++;
        // End of chain when this SQE has neither IO_LINK nor IO_HARDLINK.
        if (!(flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK))) break;
    }

    if (!first) return 0;

    // Push the head onto the worker's Treiber stack.  The chain_next
    // links the rest; worker walks both dimensions.
    io_wq_work_t* old = (io_wq_work_t*)__atomic_load_n(
        &uring->wq_head, __ATOMIC_RELAXED);
    do {
        first->next = old;
    } while (!__atomic_compare_exchange_n(&uring->wq_head, &old, first,
                                            0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
    wait_queue_wake_all(&uring->wq_waitq);
    return 0;
}

// Returns the op result; does NOT post a CQE (caller does).
// Split from dispatch_one so linked-chain logic can decide whether
// to cancel the rest of the chain based on res < 0 + IO_LINK flag.
static int32_t dispatch_exec(io_uring_t* uring, const io_sqe_t* sqe) {
    int32_t res = -EINVAL;
    switch (sqe->opcode) {
        case IORING_OP_NOP:
            res = 0;
            break;

        case IORING_OP_READ: {
            vfs_file_t* f = sqe_fdget(uring, sqe);
            if (!f) { res = -EBADF; break; }
            // Use pread-with-offset if the driver supports it (avoids
            // racy seek+read on shared fds); else fall back to
            // sequential read at the fd's current pos.
            if (f->pread && sqe->off != (uint64_t)-1)
                res = (int32_t)f->pread(f, (void*)sqe->addr, sqe->len, sqe->off);
            else
                res = (int32_t)vfs_read(f, (void*)sqe->addr, sqe->len);
            uring_fdput(f);
            break;
        }

        case IORING_OP_WRITE: {
            vfs_file_t* f = sqe_fdget(uring, sqe);
            if (!f) { res = -EBADF; break; }
            res = (int32_t)vfs_write(f, (const void*)sqe->addr, sqe->len);
            uring_fdput(f);
            break;
        }

        case IORING_OP_CLOSE: {
            // Close the fd in the current task's fd table.  sys_close
            // returns 0 on success, -errno otherwise (cast via
            // uint64_t to int32_t preserves the error in sign).
            res = (int32_t)(int64_t)sys_close((uint64_t)sqe->fd);
            break;
        }

        // ── Phase 8H: additional opcodes ─────────────────────────
        case IORING_OP_OPENAT: {
            // sqe->addr = user path pointer
            // sqe->op_flags = open flags (O_RDONLY, O_CREAT, ...)
            // sqe->len = mode (for O_CREAT)
            extern uint64_t sys_open(uint64_t, uint64_t, uint64_t);
            uint64_t r = sys_open(sqe->addr, (uint64_t)sqe->op_flags,
                                    (uint64_t)sqe->len);
            res = (int32_t)(int64_t)r;
            break;
        }

        case IORING_OP_FSYNC: {
            // No direct sys_fsync in current kernel; fsync is a no-op
            // because writes are synchronous through bcache+AHCI
            // (we don't buffer dirty pages at the VFS level yet).
            // Return 0 to satisfy callers that expect durability.
            (void)sqe;
            res = 0;
            break;
        }

        case IORING_OP_SEND: {
            // sqe->fd = socket, sqe->addr = buf, sqe->len = bytes,
            // sqe->op_flags = send flags (unused for now).
            extern uint64_t sys_sendto(uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t);
            uint64_t r = sys_sendto((uint64_t)sqe->fd, sqe->addr,
                                      (uint64_t)sqe->len,
                                      (uint64_t)sqe->op_flags,
                                      0, 0);
            res = (int32_t)(int64_t)r;
            break;
        }

        case IORING_OP_RECV: {
            extern uint64_t sys_recvfrom(uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t);
            uint64_t r = sys_recvfrom((uint64_t)sqe->fd, sqe->addr,
                                        (uint64_t)sqe->len,
                                        (uint64_t)sqe->op_flags,
                                        0, 0);
            res = (int32_t)(int64_t)r;
            break;
        }

        case IORING_OP_CONNECT: {
            // sqe->addr = sockaddr ptr, sqe->off = addrlen
            extern uint64_t sys_connect(uint64_t, uint64_t, uint64_t);
            uint64_t r = sys_connect((uint64_t)sqe->fd, sqe->addr,
                                       sqe->off);
            res = (int32_t)(int64_t)r;
            break;
        }

        case IORING_OP_ACCEPT: {
            // sqe->addr = sockaddr ptr (out), sqe->off = socklen_t ptr (out)
            extern uint64_t sys_accept(uint64_t, uint64_t, uint64_t);
            uint64_t r = sys_accept((uint64_t)sqe->fd, sqe->addr,
                                      sqe->off);
            res = (int32_t)(int64_t)r;
            break;
        }

        // IORING_OP_TIMEOUT and IORING_OP_POLL_ADD require more
        // plumbing (timer integration / poll waitqueue attach) —
        // they'd land in a follow-up phase.  Return -EINVAL for now
        // so a caller that uses them sees a clear error code.

        case IORING_OP_DRM_COMMIT: {
            // Zero-syscall DRM atomic commit.  sqe->fd resolves to the
            // DRM chardev fd; sqe->addr is the user ptr to
            // drm_mode_atomic_t.  Reuses the same commit primitive as
            // the legacy DRM_IOCTL_MODE_ATOMIC ioctl.
            vfs_file_t* f = sqe_fdget(uring, sqe);
            if (!f) { res = -EBADF; break; }
            extern int drm_ring_atomic(vfs_file_t*, uint64_t);
            res = drm_ring_atomic(f, sqe->addr);
            break;
        }

        default:
            res = -EINVAL;
            break;
    }
    return res;
}

// Thin wrapper: exec + post CQE.  Used by code paths that don't
// care about linked-chain behaviour (inline NOP batches etc.).
static void dispatch_one(io_uring_t* uring, const io_sqe_t* sqe) {
    int32_t res = dispatch_exec(uring, sqe);
    io_uring_post_cqe(uring, sqe->user_data, res, 0);
}

// Returns 1 if the opcode is known to potentially block waiting for
// I/O (disk read completion, socket accept, recv-with-no-data,
// connect handshake).  Used by the auto-async policy: these
// opcodes auto-route to io_wq unless IOSQE_FORCE_SYNC is set.
// Sync ops (NOP, CLOSE, OPENAT, FSYNC, WRITE, SEND) run inline.
static inline int opcode_may_block(uint8_t op) {
    switch (op) {
        case IORING_OP_READ:
        case IORING_OP_ACCEPT:
        case IORING_OP_RECV:
        case IORING_OP_CONNECT:
            return 1;
        default:
            return 0;
    }
}

int io_uring_enter_impl(io_uring_t* uring, uint32_t to_submit,
                         uint32_t min_complete, uint32_t flags) {
    if (!uring) return -EINVAL;

    // Phase 8F: wake the SQPOLL kthread if it's parked.  Caller bumps
    // sq_tail, then calls enter(ENTER_SQ_WAKEUP) on observing
    // IORING_SQ_NEED_WAKEUP set.
    if ((flags & IORING_ENTER_SQ_WAKEUP) && uring->sqp_task) {
        wait_queue_wake_all(&uring->sqp_waitq);
        // SQPOLL rings don't do synchronous submission from enter —
        // the poller handles it.  We only care about GETEVENTS below.
        if (!(flags & IORING_ENTER_GETEVENTS)) return 0;
    }

    uint32_t submitted = 0;
    uint32_t mask = uring->sq_hdr->ring_mask;

    // Snapshot user's sq_tail with acquire — pairs with the user's
    // release store after filling the SQE.
    uint32_t user_tail = __atomic_load_n(&uring->sq_hdr->tail, __ATOMIC_ACQUIRE);
    uint32_t head      = uring->sq_hdr->head;

    while (submitted < to_submit && head != user_tail) {
        const io_sqe_t* sqe = &uring->sqes[head & mask];

        // Auto-async policy: ops that may block go through io_wq
        // unless the user explicitly requested sync.  Covers the
        // real "async" value-add — the caller doesn't block on
        // disk I/O / network accept/recv.
        int go_async = (sqe->flags & IOSQE_ASYNC)
                    || (opcode_may_block(sqe->opcode)
                        && !(sqe->flags & IOSQE_FORCE_SYNC));

        if (go_async) {
            // Offload the SQE — and any linked followers — as a
            // single chain work item.  The worker walks the chain
            // sequentially; IO_LINK failure cancels the tail.
            int r = io_wq_enqueue_chain(uring, &head, user_tail, mask);
            if (r < 0) {
                io_uring_post_cqe(uring, sqe->user_data, r, 0);
                head++;
            }
            submitted++;
            // io_wq_enqueue_chain advances `head` past the whole
            // chain; we count the whole chain as one submission
            // here for the to_submit budget (matching Linux's
            // submitted-count semantics on linked ops).
            continue;
        }

        // Sync dispatch with IO_LINK / IO_HARDLINK semantics.
        int32_t res = dispatch_exec(uring, sqe);
        io_uring_post_cqe(uring, sqe->user_data, res, 0);
        uint8_t this_flags = sqe->flags;
        head++;
        submitted++;

        // IO_LINK + failure → cancel the rest of the chain by
        // posting -ECANCELED for each remaining linked SQE.
        // Stop at the first SQE without IO_LINK OR IO_HARDLINK
        // set (that's the end of this chain).  IO_HARDLINK is
        // special: we still post -ECANCELED for the linked SQEs
        // AFTER the hardlink boundary (Linux's exact rule) since
        // hardlink means "this runs regardless but subsequent
        // links depend on this one".
        if ((this_flags & IOSQE_IO_LINK) && res < 0) {
            while (submitted < to_submit && head != user_tail) {
                const io_sqe_t* nxt = &uring->sqes[head & mask];
                io_uring_post_cqe(uring, nxt->user_data, -ECANCELED, 0);
                uint8_t nxt_flags = nxt->flags;
                head++;
                submitted++;
                // Stop when we fall off the chain: neither this
                // SQE nor its predecessor had LINK/HARDLINK set.
                if (!(nxt_flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)))
                    break;
            }
        }
    }
    // Publish new sq_head to user.
    __atomic_store_n(&uring->sq_hdr->head, head, __ATOMIC_RELEASE);

    // GETEVENTS: block until the CQ has min_complete entries waiting
    // for userspace.  WAIT_EVENT handles the canonical "register →
    // re-check → sleep → remove" protocol, including wake_pending
    // interlock.  Woken by io_uring_post_cqe via wait_queue_wake_all.
    if (flags & IORING_ENTER_GETEVENTS) {
        WAIT_EVENT(&uring->waitq, ({
            uint32_t ch = __atomic_load_n(&uring->cq_hdr->head, __ATOMIC_ACQUIRE);
            uint32_t ct = __atomic_load_n(&uring->cq_hdr->tail, __ATOMIC_ACQUIRE);
            (ct - ch) >= min_complete;
        }));
    }

    return (int)submitted;
}
