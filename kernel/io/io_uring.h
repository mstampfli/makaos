#pragma once
#include "common.h"
#include "smp.h"          // spinlock_t
#include "wait.h"         // wait_queue_t

// ── io_uring — batched async I/O via shared ring buffers (Phase 8) ────
//
// A userspace process creates a ring via io_uring_setup, gets back a
// shared memory region with:
//   - SQ ring header (head, tail, mask, entries)
//   - CQ ring header
//   - Fixed-size SQE array (one slot per submission)
//   - Fixed-size CQE array (one slot per completion)
//
// Userspace submits ops by writing to the next SQE slot and bumping
// sq_tail (single-producer for now — only the owning task submits).
// io_uring_enter kicks the kernel SQ processor.  Kernel reads ops,
// dispatches each one, posts a CQE on completion.  Userspace reads
// completions by dequeueing from cq_head.
//
// Completion delivery is atomic: the kernel writes the CQE body THEN
// bumps cq_tail with release ordering.  Userspace reads cq_tail with
// acquire, then reads the CQE body.
//
// ── Submission Queue Entry (SQE) — 64 bytes ────────────────────────
typedef struct {
    uint8_t  opcode;         // IORING_OP_*
    uint8_t  flags;          // IOSQE_* flags (IO_LINK, FIXED_FILE, etc.)
    uint16_t ioprio;         // reserved for now
    int32_t  fd;             // target fd
    uint64_t off;            // byte offset (read/write) or reserved
    uint64_t addr;           // user buffer pointer (or iovec ptr etc.)
    uint32_t len;            // byte count
    uint32_t op_flags;       // op-specific flags (e.g. RWF_*)
    uint64_t user_data;      // opaque cookie echoed in CQE
    uint64_t reserved[3];    // padding to 64 bytes
} __attribute__((packed)) io_sqe_t;

_Static_assert(sizeof(io_sqe_t) == 64, "io_sqe_t must be 64 bytes");

// ── Completion Queue Entry (CQE) — 16 bytes ────────────────────────
typedef struct {
    uint64_t user_data;     // echoed from SQE
    int32_t  res;           // syscall return value or -errno
    uint32_t flags;         // IORING_CQE_F_* (buffer-selection, etc.)
} __attribute__((packed)) io_cqe_t;

_Static_assert(sizeof(io_cqe_t) == 16, "io_cqe_t must be 16 bytes");

// Overflow CQE storage (forward-declared in io_uring_t above).
// Small — one cqe + one next-pointer per node.
struct io_overflow_cqe {
    struct io_overflow_cqe* next;
    io_cqe_t                cqe;
};

// ── Opcodes (subset; more added in Phase 8H) ────────────────────────
enum {
    IORING_OP_NOP      = 0,
    IORING_OP_READ     = 1,
    IORING_OP_WRITE    = 2,
    IORING_OP_CLOSE    = 3,
    IORING_OP_FSYNC    = 4,
    IORING_OP_OPENAT   = 5,
    IORING_OP_ACCEPT   = 6,
    IORING_OP_CONNECT  = 7,
    IORING_OP_SEND     = 8,
    IORING_OP_RECV     = 9,
    IORING_OP_TIMEOUT  = 10,
    IORING_OP_POLL_ADD  = 11,
    IORING_OP_DRM_COMMIT = 12,   // atomic DRM modeset commit
                                  // sqe->fd = /dev/dri/card0 fd
                                  // sqe->addr = user ptr to drm_mode_atomic_t
    IORING_OP_LAST,
};

// ── Setup flags ─────────────────────────────────────────────────────
#define IORING_SETUP_SQPOLL      (1u << 0)    // Phase 8F
#define IORING_SETUP_IOPOLL      (1u << 1)    // reserved

// ── io_uring_register opcodes (Phase 8G) ───────────────────────────
#define IORING_REGISTER_FILES        0  // arg = int[] of fds, len = count
#define IORING_UNREGISTER_FILES      1  // arg = NULL, len = 0

// ── Enter flags ─────────────────────────────────────────────────────
#define IORING_ENTER_GETEVENTS   (1u << 0)    // wait for min_complete CQEs
#define IORING_ENTER_SQ_WAKEUP   (1u << 1)    // wake SQPOLL if parked

// ── SQE flags ──────────────────────────────────────────────────────
#define IOSQE_FIXED_FILE         (1u << 0)    // SQE's fd is an index into fixed files (Phase 8G)
#define IOSQE_ASYNC              (1u << 1)    // Phase 8E: force io_wq worker dispatch
#define IOSQE_IO_LINK            (1u << 2)    // next SQE depends on this one; fail → cancel rest
#define IOSQE_IO_HARDLINK        (1u << 3)    // next SQE runs regardless of this one's result
#define IOSQE_FORCE_SYNC         (1u << 4)    // override the auto-async-for-blocking-ops policy

// ── io_uring_params (kernel ↔ user) ─────────────────────────────────
// Filled by io_uring_setup.  sq_entries / cq_entries are requested in;
// kernel clamps to powers of two and returns actual values.  Every
// pointer is a user-space virtual address into the shared mapping.
typedef struct io_uring_params {
    uint32_t sq_entries;      // in/out: power of two
    uint32_t cq_entries;      // out: = sq_entries × 2 (at least)
    uint32_t flags;           // in: IORING_SETUP_*
    uint32_t sq_thread_cpu;   // in: ignored unless SQPOLL
    uint32_t sq_thread_idle;  // in: ms idle before SQPOLL parks
    uint32_t features;        // out: reserved
    uint32_t _resv[2];

    // User-space vaddrs of the shared regions (kernel fills these
    // on successful setup; caller reads).  All three point into one
    // contiguous VMA — saves two syscalls vs Linux's three-mmap
    // convention.
    uint64_t sq_ring_ptr;     // → {head, tail, mask, entries, flags, dropped}
    uint64_t cq_ring_ptr;     // → {head, tail, mask, entries, overflow}
    uint64_t sqes_ptr;        // → sq_entries SQEs
    uint64_t cqes_ptr;        // → cq_entries CQEs
    uint64_t _resv2;
} io_uring_params_t;

// SQ ring flags (written by kernel / read by userspace).
#define IORING_SQ_NEED_WAKEUP  (1u << 0)   // SQPOLL parked — need ENTER wakeup

// ── SQ/CQ ring header layouts (what lives at sq_ring_ptr / cq_ring_ptr) ─
typedef struct {
    volatile uint32_t head;         // kernel reads; advances as it processes
    volatile uint32_t tail;         // userspace bumps after writing SQE
    uint32_t          ring_mask;    // entries - 1 (power of two)
    uint32_t          ring_entries;
    volatile uint32_t flags;        // IORING_SQ_NEED_WAKEUP when SQPOLL parked
    volatile uint32_t dropped;      // SQEs dropped due to invalid opcode etc.
    // Followed by sq_array: uint32_t[] indices into SQE array.
    // (For simplicity we assume direct layout: sq_tail & mask indexes
    // SQEs directly — we don't use the indirection array yet.)
} io_sq_ring_hdr_t;

typedef struct {
    volatile uint32_t head;         // userspace bumps after reading CQE
    volatile uint32_t tail;         // kernel bumps after writing CQE
    uint32_t          ring_mask;
    uint32_t          ring_entries;
    volatile uint32_t overflow;     // CQEs dropped because CQ was full
    volatile uint32_t flags;        // reserved
} io_cq_ring_hdr_t;

// ── Kernel-internal io_uring instance ───────────────────────────────
struct mm_t;
struct task_t;
struct vfs_file_t;

// ── io_wq work item (Phase 8E + 8I linked chains) ──────────────────
// Queued onto the ring's per-worker pending list (Treiber stack).
// Allocated from a dedicated slab cache so enqueue is a single CAS.
//
// Linked ops (IOSQE_IO_LINK / IOSQE_IO_HARDLINK) are represented as
// a chain: `chain_next` points to the next link, NULL at the end.
// The worker walks the chain sequentially; on IO_LINK failure it
// cancels the tail by posting -ECANCELED for each remaining item
// (stopping at the first HARDLINK boundary).
struct io_wq_work;
typedef struct io_wq_work {
    struct io_wq_work* next;        // Treiber stack link (queue side)
    struct io_wq_work* chain_next;  // linked-chain next (NULL = chain end)
    io_sqe_t           sqe;         // SQE snapshot (SQ slot is reused)
    struct io_uring*   uring;       // back-pointer for CQE post
} io_wq_work_t;

typedef struct io_uring {
    // Backing storage (contiguous phys pages) and the user vaddr it's
    // mapped at in the creating task's mm.
    phys_addr_t  backing_phys;
    uint64_t     backing_bytes;
    virt_addr_t  user_vaddr;         // base of user-visible mapping
    struct mm_t* owner_mm;           // task that called setup — used to
                                     // walk user pointers later
    struct task_t* owner_task;       // hint for waking waiter

    // ── Phase 8E: async worker ─────────────────────────────────
    // Single kthread per ring — shares mm_shared + files_shared
    // with owner_task so it sees the same user memory and fd
    // table.  Queue is a lockless Treiber stack; worker drains
    // via atomic_exchange.
    struct task_t*         worker;     // kthread spawned on first async op
    struct io_wq_work* volatile wq_head;  // pending work (Treiber)
    wait_queue_t           wq_waitq;    // worker sleeps here when head==NULL
    volatile uint32_t      worker_stop; // set on ring close
    volatile uint32_t      worker_done; // worker's last write before TASK_DEAD

    // ── Phase 8F: SQPOLL kthread ───────────────────────────────
    // When IORING_SETUP_SQPOLL was requested, a dedicated poller
    // kthread watches the SQ for new submissions — userspace can
    // write SQEs and bump sq_tail without ever calling
    // io_uring_enter.  After sq_thread_idle ms of no work the
    // poller parks on sqp_waitq; user sets IORING_SQ_NEED_WAKEUP
    // and calls enter with ENTER_SQ_WAKEUP to unpark.
    struct task_t*    sqp_task;        // NULL if not SQPOLL mode
    wait_queue_t      sqp_waitq;
    volatile uint32_t sqp_idle_ms;     // sq_thread_idle
    volatile uint32_t sqp_stop;
    volatile uint32_t sqp_done;        // poller's last write before TASK_DEAD

    // ── Phase 8G: fixed files ──────────────────────────────────
    // User registers an array of fds once; subsequent SQEs with
    // IOSQE_FIXED_FILE pass sqe->fd as INDEX into fixed_files[]
    // instead of as a real fd.  Skips the RCU-protected fd-table
    // lookup + tryget on every op — measurable for high-rate
    // workloads (1 atomic per op adds up).
    struct vfs_file_t** fixed_files;    // NULL = no registration
    uint32_t            fixed_files_nr;
    spinlock_t          fixed_files_lock;

    // ── CQ overflow list (Phase 8 follow-up) ───────────────────
    // When the CQ is full, CQEs would be dropped — instead they
    // go onto a kernel-side singly-linked list.  Next post_cqe
    // that finds CQ space drains the list head-first before
    // publishing new CQEs.  Matches Linux io_uring's overflow
    // handling.
    struct io_overflow_cqe* overflow_head;
    struct io_overflow_cqe* overflow_tail;
    spinlock_t              overflow_lock;

    // Kernel-side aliases (HHDM) into the backing memory — lets the
    // kernel read/write ring fields without going through the user
    // mapping.  Faster and avoids invalidating user-space TLB.
    io_sq_ring_hdr_t* sq_hdr;
    io_cq_ring_hdr_t* cq_hdr;
    io_sqe_t*         sqes;           // sq_entries elements
    io_cqe_t*         cqes;           // cq_entries elements
    uint32_t          sq_entries;
    uint32_t          cq_entries;
    uint32_t          flags;

    // Lock for posting CQEs when multiple workers are active (Phase 8E).
    spinlock_t   cq_lock;

    // Wait queue: a task blocked in io_uring_enter with
    // IORING_ENTER_GETEVENTS sleeps here; the SQ processor wakes
    // them after posting the requested number of CQEs.
    wait_queue_t waitq;
} io_uring_t;

// ── Public API ─────────────────────────────────────────────────────
// Create a ring instance.  On success, maps it into the caller's mm
// and fills user_params with offsets/pointers.  Returns a vfs_file_t
// for fd installation, or NULL on OOM/EINVAL.
struct vfs_file_t* io_uring_create(uint32_t entries,
                                    io_uring_params_t* kparams);

// Kick the SQ processor.  Walks SQEs from sq_head to user's sq_tail
// and dispatches each.  Returns number submitted (n_submitted may be
// less if an opcode was invalid).
//
// If flags & IORING_ENTER_GETEVENTS: blocks until cq_tail - cq_head
// >= min_complete.
int io_uring_enter_impl(io_uring_t* uring, uint32_t to_submit,
                         uint32_t min_complete, uint32_t flags);

// Post a CQE on behalf of a completed op.  Safe to call from any
// context (takes cq_lock internally).
void io_uring_post_cqe(io_uring_t* uring, uint64_t user_data,
                        int32_t res, uint32_t cqe_flags);

// Phase 8G: register/unregister per-ring resources.  `op` is
// IORING_REGISTER_* / IORING_UNREGISTER_*.  `arg` is a user ptr
// (int[] for files), `nr_args` the count.  Returns 0 or -errno.
int io_uring_register_impl(io_uring_t* uring, uint32_t op,
                            uint64_t arg_uptr, uint32_t nr_args);
