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
// Cap on the CQ-overflow list length.  A ring whose CQ is full spills extra
// completions onto a kmalloc'd singly-linked list; an unprivileged ring that
// never reaps its CQ would otherwise grow it without bound (kernel-memory DoS).
// Past the cap, completions drop + bump the user-visible overflow counter (the
// same last-resort behavior as the OOM branch).  ~96 KiB max overflow per ring.
#define IO_URING_OVERFLOW_MAX 4096u

// Forward declarations for helpers used across phases.
static int     io_sqp_spawn(io_uring_t* uring);
static int     io_wq_enqueue(io_uring_t* uring, const io_sqe_t* sqe);
// Consumes the chain at *phead and posts exactly one CQE per SQE; on a failure
// to enqueue it completes the whole chain with error CQEs.  Returns void: the
// caller has nothing to post or advance afterward.
static void    io_wq_enqueue_chain(io_uring_t* uring, uint32_t* phead,
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

// Pure: mask a raw ring counter to a valid index.  For power-of-two `entries`
// the result is always in [0, entries) regardless of the raw value.  Unit-tested.
static inline uint32_t io_ring_index(uint32_t raw, uint32_t entries) {
    return raw & (entries - 1u);
}

// Kernel-TRUSTED ring index masks.  sq_entries/cq_entries are set by setup from
// round_pow2_u32(...) clamped to IO_URING_MAX_ENTRIES -- powers of two, stored
// in the kernel io_uring_t, NEVER user-writable.  Index sqes[]/cqes[] with
// these.  NEVER use sq_hdr->ring_mask / cq_hdr->ring_mask for indexing: those
// live in the user-mapped ring header (vmm_map_physical_user, VMA_R|W|USER) and
// a malicious process can overwrite them to 0xFFFFFFFF, turning `idx & mask`
// into an unmasked OOB sqe read / attacker-controlled OOB cqe write.
static inline uint32_t io_sq_mask(const io_uring_t* u) { return u->sq_entries - 1u; }
static inline uint32_t io_cq_mask(const io_uring_t* u) { return u->cq_entries - 1u; }

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
        // Phase 8E/8F: stop AND JOIN the worker + SQPOLL kthreads before
        // we free anything.  Both kthreads dereference `uring` (the Treiber
        // queue, the SQ/CQ ring, owner_task, the backing pages) right up
        // until they observe their stop flag and exit.  Freeing `uring` or
        // pmm_buddy_free()ing the backing while a kthread is still draining
        // is a use-after-free: the freed slab/pages get reused (commonly as
        // page tables or another task_mm_t), and the dangling kthread then
        // walks a garbage work-chain / reads a poisoned descriptor — which
        // surfaced as a non-deterministic #PF/#GP on a corrupt mm_shared in
        // do_switch / the page-fault handler under the io_uring bench.
        //
        // Each kthread publishes *_done as its LAST act before TASK_DEAD, so
        // once we observe it set, the kthread provably touches `uring` no
        // more and it is safe to free.  We re-wake every spin to close the
        // park-after-stop lost-wakeup window, and sched_yield so the kthread
        // (typically on another CPU) makes progress.  close()/task-exit both
        // run in process context, so yielding here is safe.
        // Stop + JOIN the SQPOLL kthread FIRST.  It is the only io_wq-worker
        // spawn site that runs WITHOUT an fd ref (io_uring_enter holds an fdget
        // ref, so it cannot race this last-ref-drop close); until SQPOLL is
        // provably dead it can still io_wq_ensure_worker() on an IOSQE_ASYNC SQE
        // and publish uring->worker AFTER we read it -- an orphaned worker that
        // never sees worker_stop and then UAFs the freed ring.  Joining SQPOLL
        // first makes uring->worker stable before we decide to stop/join it.
        if (uring->sqp_task)
            __atomic_store_n(&uring->sqp_stop, 1u, __ATOMIC_RELEASE);
        while (uring->sqp_task &&
               !__atomic_load_n(&uring->sqp_done, __ATOMIC_ACQUIRE)) {
            wait_queue_wake_all(&uring->sqp_waitq);
            sched_yield();
        }
        // SQPOLL is dead now -> read uring->worker with acquire (observes a
        // worker SQPOLL may have published just before exiting), then stop+join.
        if (__atomic_load_n(&uring->worker, __ATOMIC_ACQUIRE))
            __atomic_store_n(&uring->worker_stop, 1u, __ATOMIC_RELEASE);
        while (__atomic_load_n(&uring->worker, __ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&uring->worker_done, __ATOMIC_ACQUIRE)) {
            wait_queue_wake_all(&uring->wq_waitq);
            sched_yield();
        }
        // Phase 8G: release fixed files.
        if (uring->fixed_files) {
            for (uint32_t i = 0; i < uring->fixed_files_nr; i++)
                if (uring->fixed_files[i]) vfs_close(uring->fixed_files[i]);
            kfree(uring->fixed_files);
            uring->fixed_files = NULL;
        }
        // ── Unmap the backing from the owner's address space BEFORE we
        // return its physical pages to the buddy allocator.  On an explicit
        // close(2) — the common case — the user VMA is still live (it is
        // only auto-torn-down at task exit).  Freeing the pages first lets
        // the buddy re-hand them out (as a kstack, page table, or a fresh
        // ring's backing) while the stale user mapping still aliases them:
        // a use-after-free that scribbles the reused victim.  io_uring_create
        // even memset()s a freshly allocated backing to zero — so a reused
        // page that is now a live kstack gets a saved register zeroed, which
        // surfaced as a NULL cpu_t in do_switch (and as a poisoned task_mm_t).
        //
        // We can only touch the mapping from the owning address space.
        // close(2) and task-exit fd cleanup both run in it (g_current's mm is
        // the one the ring was created in), which covers every real caller.
        // If some other AS closes an inherited ring fd we skip the unmap and
        // leak the mapping rather than risk freeing live, aliased pages.
        uint64_t npg = uring->backing_bytes >> PAGE_SHIFT;
        if (uring->user_vaddr && uring->owner_mm && g_current &&
            g_current->mm_shared &&
            g_current->mm_shared->mm == uring->owner_mm) {
            extern uint8_t mm_vma_remove(struct mm_t*, uint64_t, uint64_t);
            extern void tlb_flush_range(task_mm_t*, uint64_t, uint64_t);
            phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
            for (uint64_t i = 0; i < npg; i++) {
                phys_addr_t fr = 0;
                vmm_page_unmap(pml4, uring->user_vaddr + i * PAGE_SIZE, &fr);
            }
            mm_vma_remove(uring->owner_mm, uring->user_vaddr,
                          uring->user_vaddr + uring->backing_bytes);
            tlb_flush_range(g_current->mm_shared, uring->user_vaddr,
                            uring->user_vaddr + uring->backing_bytes);
            uring->user_vaddr = 0;
        }
        // Drain any pending CQ-overflow nodes -- the drain-on-post path is the
        // ONLY other freer and it only runs while the ring is open, so a ring
        // closed with a non-empty overflow list would leak every node.  The
        // SQPOLL + worker kthreads are joined above and the fd is gone, so no
        // poster races this (the overflow_lock is taken for symmetry).
        {
            uint64_t of = spin_lock_irqsave(&uring->overflow_lock);
            struct io_overflow_cqe* n = uring->overflow_head;
            while (n) { struct io_overflow_cqe* nx = n->next; kfree(n); n = nx; }
            uring->overflow_head = uring->overflow_tail = NULL;
            uring->overflow_count = 0;
            spin_unlock_irqrestore(&uring->overflow_lock, of);
        }
        // Now that no mapping aliases them, return the backing pages.
        uint8_t order = 0;
        while ((1ULL << order) < npg) order++;
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
    uring->worker_done = 0;
    spin_lock_init(&uring->worker_lock);
    uring->sqp_task = NULL;
    uring->sqp_stop = 0;
    uring->sqp_done = 0;
    uring->sqp_idle_ms = kparams->sq_thread_idle ? kparams->sq_thread_idle : 1000;
    uring->fixed_files    = NULL;
    uring->fixed_files_nr = 0;
    spin_lock_init(&uring->fixed_files_lock);
    uring->overflow_head = NULL;
    uring->overflow_tail = NULL;
    uring->overflow_count = 0;
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
    uint32_t mask = io_cq_mask(uring);   // TRUSTED count, not the user-writable header

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
            uring->overflow_count--;
            kfree(n);
        }
        spin_unlock_irqrestore(&uring->overflow_lock, of);
    }

    uint32_t tail = uring->cq_hdr->tail;
    uint32_t head = __atomic_load_n(&uring->cq_hdr->head, __ATOMIC_ACQUIRE);

    if ((tail - head) >= uring->cq_entries) {
        // CQ still full -- push onto the overflow list instead of dropping.
        // BUT cap the list: a ring that never reaps its CQ would otherwise grow
        // it without bound (kernel-memory DoS).  Past the cap, drop + count
        // (the same last-resort behavior as the OOM branch below); userspace
        // sees cq_hdr->overflow and reaps.
        if (uring->overflow_count >= IO_URING_OVERFLOW_MAX) {
            uring->cq_hdr->overflow++;
            spin_unlock_irqrestore(&uring->cq_lock, f);
            wait_queue_wake_all(&uring->waitq);
            return;
        }
        // Bump the user-visible overflow counter as a hint (userspace can
        // monitor it to throttle submissions).
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
        uring->overflow_count++;
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
        // Hold fixed_files_lock across the (nr, array[idx]) read AND the
        // tryget.  This runs on the io_wq worker kthread concurrently with the
        // owner task's IORING_UNREGISTER_FILES, which swaps fixed_files->NULL /
        // nr->0 under this lock and THEN (outside it) vfs_close's each file and
        // kfree's the array.  Without the lock the worker can read a freed
        // array, see a torn nr-vs-array, or tryget a file whose backing object
        // unregister already freed (a UAF / freed-function-pointer call via the
        // file's vtable).  Under the lock we read a consistent table and
        // vfs_tryget pins the file (its own ref) BEFORE unregister can drop the
        // ring's ref, so the file cannot be freed under us; and we never index
        // the array after unregister NULLed it (nr is 0 then -> return NULL).
        uint64_t fl = spin_lock_irqsave(&uring->fixed_files_lock);
        vfs_file_t* f = (idx < uring->fixed_files_nr) ? uring->fixed_files[idx]
                                                      : (vfs_file_t*)NULL;
        f = f ? vfs_tryget(f) : NULL;
        spin_unlock_irqrestore(&uring->fixed_files_lock, fl);
        return f;
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
            // Last touch of `uring`: publish done so io_uring_close_file can
            // safely free it (see the join in io_uring_close_file).
            __atomic_store_n(&uring->sqp_done, 1u, __ATOMIC_RELEASE);
            g_current->state = TASK_DEAD;
            sched_yield();
            return;
        }

        uint32_t mask = io_sq_mask(uring);   // TRUSTED count, not the user header
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
                    // On enqueue failure (no worker / wq_work_alloc OOM) the SQE
                    // is still consumed below; post an error CQE so the slot
                    // completes -- else a GETEVENTS waiter on min_complete hangs.
                    int r = io_wq_enqueue(uring, sqe);
                    if (r < 0) io_uring_post_cqe(uring, sqe->user_data, r, 0);
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
                // Last touch of `uring`: publish done so io_uring_close_file
                // can safely free it (see the join in io_uring_close_file).
                __atomic_store_n(&uring->worker_done, 1u, __ATOMIC_RELEASE);
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
    if (__atomic_load_n(&uring->worker, __ATOMIC_ACQUIRE)) return 0;
    if (!uring->owner_task) return -EINVAL;

    // Serialise the check-create-publish: two concurrent consumers (a non-SQPOLL
    // ring driven by two threads sharing its fd) would both observe worker==NULL
    // and each spawn a kthread; the second store would orphan the first, and
    // io_uring_close_file joins only the published worker -> the orphan runs on
    // and touches the freed `uring` (use-after-free).  Double-checked locking:
    // re-test under the lock so exactly ONE worker is ever created.  Held only
    // on the first async op; task_create_kthread / sched_add do not sleep and
    // never take this lock, so no sleep-under-spinlock and no cycle.
    spin_lock(&uring->worker_lock);
    if (__atomic_load_n(&uring->worker, __ATOMIC_ACQUIRE)) {
        spin_unlock(&uring->worker_lock);
        return 0;
    }

    task_t* t = task_create_kthread(io_wq_worker_entry, pid_alloc());
    if (!t) { spin_unlock(&uring->worker_lock); return -ENOMEM; }

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

    // Stash the ring pointer so the worker can find it.  Publish uring->worker
    // with RELEASE so io_uring_close_file's ACQUIRE read (after it has joined
    // SQPOLL, this worker's spawner) is guaranteed to observe it and join it.
    t->kthread_ctx = uring;
    __atomic_store_n(&uring->worker, t, __ATOMIC_RELEASE);
    sched_add(t);
    spin_unlock(&uring->worker_lock);
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

// A chain could not be enqueued: complete + CONSUME the whole chain starting at
// *phead.  Posts one CQE per SQE -- the head op gets `err`, each linked follower
// gets -ECANCELED (mirroring the sync path's IO_LINK-failure cancel at the
// dispatch loop) -- and advances *phead past the chain.  This preserves the
// SQE:CQE 1:1 invariant: without it, an ENOMEM mid-submit consumed SQEs with no
// CQE, hanging a thread blocked in io_uring_enter(GETEVENTS) on min_complete.
static void io_wq_chain_fail(io_uring_t* uring, uint32_t* phead,
                             uint32_t user_tail, uint32_t mask, int err) {
    int first = 1;
    while (*phead != user_tail) {
        const io_sqe_t* sqe = &uring->sqes[*phead & mask];
        io_uring_post_cqe(uring, sqe->user_data, first ? err : -ECANCELED, 0);
        first = 0;
        uint8_t flags = sqe->flags;
        (*phead)++;
        if (!(flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK))) break;
    }
}

// Enqueue an SQE and any following IO_LINK/IO_HARDLINK SQEs as one
// chained work item.  *phead is advanced past the whole chain.
// The worker walks chain_next sequentially with IO_LINK cancel
// semantics (see worker loop).  On any failure to enqueue (no worker, or a
// mid-build OOM) the chain is completed with error CQEs and consumed, so the
// caller never has to post or advance -- and no completion is ever lost.
static void io_wq_enqueue_chain(io_uring_t* uring, uint32_t* phead,
                                 uint32_t user_tail, uint32_t mask) {
    int r = io_wq_ensure_worker(uring);
    if (r < 0) {
        // No worker -> the whole chain cannot run; complete + consume it.
        io_wq_chain_fail(uring, phead, user_tail, mask, r);
        return;
    }

    // Build the chain into a LOCAL cursor; *phead is committed only once the
    // whole chain is built (success) or by io_wq_chain_fail (failure) -- never
    // left half-advanced past freed-but-uncompleted SQEs (the old bug).
    uint32_t h = *phead;
    io_wq_work_t* first = NULL;
    io_wq_work_t* tail  = NULL;

    for (;;) {
        if (h == user_tail) break;
        const io_sqe_t* sqe = &uring->sqes[h & mask];

        io_wq_work_t* w = wq_work_alloc();
        if (!w) {
            // Partial-build OOM: free what we built, then complete + consume
            // the WHOLE chain from the original *phead with error CQEs.
            while (first) {
                io_wq_work_t* n = first->chain_next;
                wq_work_free(first);
                first = n;
            }
            io_wq_chain_fail(uring, phead, user_tail, mask, -ENOMEM);
            return;
        }
        w->sqe        = *sqe;
        w->uring      = uring;
        w->chain_next = NULL;
        if (!first) first = w;
        if (tail)   tail->chain_next = w;
        tail = w;

        uint8_t flags = sqe->flags;
        h++;
        // End of chain when this SQE has neither IO_LINK nor IO_HARDLINK.
        if (!(flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK))) break;
    }

    if (!first) { *phead = h; return; }

    // Push the head onto the worker's Treiber stack.  The chain_next
    // links the rest; worker walks both dimensions.
    io_wq_work_t* old = (io_wq_work_t*)__atomic_load_n(
        &uring->wq_head, __ATOMIC_RELAXED);
    do {
        first->next = old;
    } while (!__atomic_compare_exchange_n(&uring->wq_head, &old, first,
                                            0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
    *phead = h;   // commit the chain's advancement only after a successful push
    wait_queue_wake_all(&uring->wq_waitq);
}

// Returns the op result; does NOT post a CQE (caller does).
// Split from dispatch_one so linked-chain logic can decide whether
// to cancel the rest of the chain based on res < 0 + IO_LINK flag.
static int32_t dispatch_exec(io_uring_t* uring, const io_sqe_t* sqe) {
    // The SQE lives in the user-WRITABLE mmap'd ring (uring->sqes[], mapped
    // VMA_R|W|USER); the sync and SQPOLL dispatch paths pass a LIVE pointer into
    // it.  Snapshot it ONCE into a kernel-local copy and read only that, so a
    // concurrent user write cannot change addr/len/fd between user_buf_check
    // (validation) and vfs_read/vfs_write (use) -- a double-fetch TOCTOU that is
    // otherwise an arbitrary kernel read/write (LPE), trivially reachable via
    // SQPOLL.  The async io_wq path already snapshots into w->sqe; this makes
    // every dispatch path single-fetch.  io_sqe_t is 64 bytes -- safe on stack.
    io_sqe_t snap = *sqe;
    sqe = &snap;
    int32_t res = -EINVAL;
    switch (sqe->opcode) {
        case IORING_OP_NOP:
            res = 0;
            break;

        case IORING_OP_READ: {
            vfs_file_t* f = sqe_fdget(uring, sqe);
            if (!f) { res = -EBADF; break; }
            // sqe->addr/len are user-controlled (read straight from the SQE
            // ring); vfs_read writes DIRECTLY into this buffer, so a kernel
            // address here would be an arbitrary kernel write.  Validate it.
            extern int user_buf_check(uint64_t addr, uint64_t len);
            if (user_buf_check(sqe->addr, sqe->len) != 0) {
                uring_fdput(f); res = -EFAULT; break;
            }
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
            // Validate the user-controlled source buffer; vfs_write reads it
            // directly, so a kernel address here is an arbitrary kernel read.
            extern int user_buf_check(uint64_t addr, uint64_t len);
            if (user_buf_check(sqe->addr, sqe->len) != 0) {
                uring_fdput(f); res = -EFAULT; break;
            }
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
            uring_fdput(f);   // sqe_fdget took a ref; balance it like READ/WRITE
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

    uint32_t submitted = 0;

    // SQPOLL rings: the poller kthread (io_sqp_kthread_entry) is the SOLE SQ
    // consumer.  enter() must NEVER consume the SQ here, REGARDLESS of flags --
    // doing so races the poller: both read the same sq_head, dispatch the same
    // SQE twice (double send/write/open + a duplicate CQE), non-atomically store
    // head back (lost update -> unbounded re-dispatch), and an async SQE makes
    // both call io_wq_ensure_worker, orphaning a worker that close never joins
    // (use-after-free).  The old guard only skipped the SQ_WAKEUP-without-
    // GETEVENTS case, so enter(GETEVENTS) and enter(0) on a SQPOLL ring fell
    // through into the consumer loop.  Just wake the poller if asked, then go
    // straight to the GETEVENTS wait below.
    if (uring->sqp_task) {
        if (flags & IORING_ENTER_SQ_WAKEUP)
            wait_queue_wake_all(&uring->sqp_waitq);
        goto getevents;
    }

    uint32_t mask = io_sq_mask(uring);   // TRUSTED count, not the user-writable header

    // Snapshot user's sq_tail with acquire -- pairs with the user's
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
            // io_wq_enqueue_chain consumes `head` past the whole chain and posts
            // exactly one CQE per SQE -- on success the worker completes them; on
            // a failure to enqueue it completes them with error CQEs here.  So
            // there is nothing to post or advance afterward (the old code posted
            // a single CQE + head++, losing the rest of a partially-built chain).
            io_wq_enqueue_chain(uring, &head, user_tail, mask);
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

getevents:
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

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic check of the ring index masking that replaced the
// user-writable ring_mask (the OOB fix): for power-of-two `entries`, a masked
// index is ALWAYS in [0, entries) regardless of the raw counter -- so a
// malicious 0xFFFFFFFF can no longer drive an out-of-bounds sqe/cqe access.
void io_uring_index_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    struct { uint32_t raw, entries, want; } c[] = {
        { 0,          4,    0    },
        { 3,          4,    3    },
        { 4,          4,    0    },   // wraps within the ring
        { 0xFFFFFFFFu,4,    3    },   // adversarial counter -> still in [0,4)
        { 0x12345u,   256,  0x45 },   // 0x12345 & 0xFF
        { 0xFFFFFFFFu,256,  255  },
        { 0xFFFFFFFFu,4096, 4095 },
        { 0xFFFFFFFFu,1,    0    },   // single-entry ring (mask 0)
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        uint32_t got = io_ring_index(c[i].raw, c[i].entries);
        if (got != c[i].want || got >= c[i].entries) {
            kprintf("[io_ring_idx] FAIL raw=0x%lx entries=%u got=%u want=%u\n",
                    (unsigned long)c[i].raw, c[i].entries, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[io_ring_idx] SELF-TEST FAILED\n"
                  : "[io_ring_idx] SELF-TEST PASSED (trusted ring index, no user-mask OOB)\n");
}
#endif /* MAKAOS_BOOT_SELFTESTS */
