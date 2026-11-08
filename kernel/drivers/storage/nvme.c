// ── NVMe driver — step 1: bring-up + Identify ───────────────────────────
//
// This is the initial bring-up: PCI discovery, map BAR0, reset the
// controller, program the admin queue, enable, then issue Identify
// Controller and Identify Namespace to prove the register programming
// is correct.  I/O queues and per-CPU distribution land in later steps.
//
// References: NVMe 1.4 specification §3.1 (Register Definition),
// §4.1–4.4 (Queue Submission & Completion), §5.15 (Identify).

#include "nvme.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "common.h"
#include "kprintf.h"
#include "lapic.h"
#include "idt.h"
#include "wait.h"
#include "sched.h"
#include "smp.h"
#include "preempt.h"
#include "cpu.h"
#include "acpi.h"
#include "checked.h"   // index_ok: single source of truth for bounded indices

// ── PCI class codes ─────────────────────────────────────────────────────
#define PCI_CLASS_STORAGE    0x01
#define PCI_SUBCLASS_NVME    0x08   // "Non-Volatile Memory Controller"
#define PCI_PROG_IF_NVME     0x02   // NVMe I/O Controller

// ── Register offsets in the controller register region (BAR0) ───────────
// See NVMe 1.4 spec §3.1.
#define NVME_REG_CAP        0x0000  // Controller Capabilities (RO, 64b)
#define NVME_REG_VS         0x0008  // Version (RO, 32b)
#define NVME_REG_INTMS      0x000C  // Interrupt Mask Set (RW1S, 32b)
#define NVME_REG_INTMC      0x0010  // Interrupt Mask Clear (RW1C, 32b)
#define NVME_REG_CC         0x0014  // Controller Configuration (RW, 32b)
#define NVME_REG_CSTS       0x001C  // Controller Status (RO, 32b)
#define NVME_REG_NSSR       0x0020  // NVM Subsystem Reset (RW, 32b)
#define NVME_REG_AQA        0x0024  // Admin Queue Attributes (RW, 32b)
#define NVME_REG_ASQ        0x0028  // Admin SQ Base Address (RW, 64b)
#define NVME_REG_ACQ        0x0030  // Admin CQ Base Address (RW, 64b)
// Doorbells start at 0x1000, stride = 4 << (CAP.DSTRD).
// SQyTDBL at 0x1000 + 2*y*stride; CQyHDBL at 0x1000 + (2*y+1)*stride.
// The doorbell offset scales with the device-reported CAP.DSTRD and the queue
// id, so BAR0 is mapped to cover the highest doorbell we may write (qid up to
// MAX_CPUS); a controller advertising a stride that would need more than this
// is refused rather than allowed to drive an MMIO store past the mapping.
#define NVME_BAR0_MAX_MAP   0x100000u   // 1 MiB ceiling on the BAR0 doorbell map

// ── CC (Controller Configuration) bit layout ────────────────────────────
#define CC_EN              (1u << 0)   // Enable
#define CC_CSS_NVM         (0u << 4)   // I/O Command Set: NVM
#define CC_MPS_4K          (0u << 7)   // Memory Page Size: 4 KiB (2^(12+MPS))
#define CC_AMS_RR          (0u << 11)  // Arbitration: round-robin
#define CC_SHN_NONE        (0u << 14)  // No shutdown requested
#define CC_IOSQES_64       (6u << 16)  // 2^6 = 64 bytes per SQE
#define CC_IOCQES_16       (4u << 20)  // 2^4 = 16 bytes per CQE

// ── CSTS (Controller Status) bits ───────────────────────────────────────
#define CSTS_RDY           (1u << 0)   // Ready
#define CSTS_CFS           (1u << 1)   // Controller Fatal Status

// ── Submission Queue Entry — 64 bytes ───────────────────────────────────
// NVMe 1.4 §4.1 Figure 13.
typedef struct {
    uint32_t opc_fuse_psdt_cid;  // [7:0]=opcode [9:8]=fuse [15]=PSDT=PRP [31:16]=CID
    uint32_t nsid;               // Namespace ID (0 for generic admin)
    uint64_t rsvd_dword_2_3;
    uint64_t mptr;               // Metadata pointer (0 for now)
    uint64_t prp1;               // Physical Region Page 1
    uint64_t prp2;               // Physical Region Page 2 (or PRP list ptr)
    uint32_t cdw10;              // Command-specific
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "SQE must be 64 bytes");

// ── Completion Queue Entry — 16 bytes ───────────────────────────────────
// NVMe 1.4 §4.6 Figure 76.
typedef struct {
    uint32_t dw0;                // Command-specific result
    uint32_t dw1;
    uint16_t sq_head;            // SQ head pointer
    uint16_t sq_id;              // SQ identifier
    uint16_t cid;                // Command identifier (echoed)
    uint16_t status_phase;       // [0]=phase, [15:1]=status
} __attribute__((packed)) nvme_cqe_t;

_Static_assert(sizeof(nvme_cqe_t) == 16, "CQE must be 16 bytes");

// ── Admin opcodes ───────────────────────────────────────────────────────
#define NVME_ADMIN_OP_DELETE_IOSQ 0x00
#define NVME_ADMIN_OP_CREATE_IOSQ 0x01
#define NVME_ADMIN_OP_DELETE_IOCQ 0x04
#define NVME_ADMIN_OP_CREATE_IOCQ 0x05
#define NVME_ADMIN_OP_IDENTIFY    0x06
#define NVME_ADMIN_OP_SET_FEAT    0x09

// ── NVM command set opcodes ─────────────────────────────────────────────
#define NVME_NVM_OP_FLUSH  0x00
#define NVME_NVM_OP_WRITE  0x01
#define NVME_NVM_OP_READ   0x02

// ── Identify CNS values ─────────────────────────────────────────────────
#define NVME_IDENT_CNS_NS         0x00  // Identify Namespace (nsid)
#define NVME_IDENT_CNS_CTRL       0x01  // Identify Controller
#define NVME_IDENT_CNS_NS_LIST    0x02  // Active Namespace List

// ── Driver state ────────────────────────────────────────────────────────
static uint8_t*        s_regs = NULL;     // BAR0 MMIO base (UC)
static uint64_t        s_cap  = 0;
static uint32_t        s_doorbell_stride = 4;  // computed from CAP.DSTRD
static uint32_t        s_max_qentries = 0;     // CAP.MQES + 1

// Admin queues (64 entries each).
#define NVME_ADMIN_QDEPTH   64

static nvme_sqe_t*     s_asq = NULL;      // admin SQ (HHDM ptr)
static nvme_cqe_t*     s_acq = NULL;      // admin CQ (HHDM ptr)
static phys_addr_t     s_asq_phys = 0;
static phys_addr_t     s_acq_phys = 0;
static uint32_t        s_asq_tail = 0;
static uint32_t        s_acq_head = 0;
static uint8_t         s_acq_phase = 1;   // phase bit flips each wrap; initial

// I/O queue pair #1.  One per step 2; step 4 scales to per-CPU.
//
// 4 KiB SQ holds 64 entries of 64 B; 4 KiB CQ holds 256 entries of 16 B.
// We use 64 entries for both — plenty for realistic depth-per-CPU.
#define NVME_IOQ_DEPTH    64
// QID 0 is admin.  I/O queues start at QID 1 and map 1:1 to CPUs:
// queue i belongs to CPU (i-1).  Sized by ACPI cpu_count at init.
#define NVME_IOQ_QID(cpu)  (uint16_t)((cpu) + 1)

static uint32_t        s_ns_nsid = 0;      // first namespace id
static uint32_t        s_ns_lba_size = 512;
static uint32_t        s_nr_ioq = 0;       // number of per-CPU I/O queues

// PRIMITIVE (nvme LBA-size validation).  The Identify-Namespace LBA Data Size
// exponent (LBADS, an 8-bit device-supplied field, 0..255) gives the sector
// size as 2^LBADS.  A bare `1u << lbads` is UNDEFINED for lbads >= 32 (x86
// masks the shift count to & 31, so e.g. lbads=32 yields 1 -- a tiny bogus
// sector size), and any sub-512 / oversized size defeats nvme_rw's
// mul_within_u32(nlb, s_ns_lba_size, 8192) DMA guard: a lying controller makes
// our byte length under-count, so a transfer slips past the 2-PRP (<=8 KiB)
// bound and the device DMAs past the PRPs.  Accept only the driver-supported
// 512..4096-byte range (lbads 9..12) and compute the size with no UB; reject
// everything else (nvme_init then refuses the namespace).  Returns true + the
// byte size in *size on success.  Pure -> unit-tested (nvme_lba_size_ok_selftest).
static inline bool nvme_lba_size_ok(uint32_t lbads, uint32_t* size) {
    if (lbads < 9u || lbads > 12u) return false;   // only 512..4096-byte sectors
    *size = 1u << lbads;                            // safe: lbads <= 12, no UB/wrap
    return true;
}

// PCI location — saved so MSI-X setup can write PCI config.
static pci_device_t    s_pci;
static volatile uint32_t* s_msix_table = NULL;  // 16 B per entry × N entries

// Per-request state — the SUBMITTER's wait object.  wait_queue_init is
// called for every new submit, so across CID reuse there is no stale
// state.  A late wake from a drained entry is harmless (sleep_we is
// task-persistent).
typedef struct nvme_request {
    wait_queue_t      wq;
    volatile uint8_t  done;
    volatile uint16_t status;
} nvme_request_t;

// ── Per-CPU I/O queue pair ──────────────────────────────────────────────
//
// Every online CPU gets its own SQ+CQ, its own CID bitmap, and its own
// MSI-X vector that fires ONLY on that CPU.  Submissions and completions
// run entirely on the same CPU: no cross-CPU state, no lock.  The
// `sq_lock` below is still taken because a task CAN be preempted inside
// io_submit_async by its own timer tick — a second task on the same
// queue's CPU would then race on sq_tail if we didn't serialise.  IRQs
// are disabled during the critical section (spin_lock_irqsave), so the
// ISR on the owning CPU can't nest here either.
//
// Aligned to cache line so two CPUs' queues don't share one line.
typedef struct nvme_ioq {
    uint16_t            qid;           // 1..s_nr_ioq
    uint16_t            cpu;           // owning cpu_id (0..MAX_CPUS-1)
    uint8_t             msix_vec;      // IDT vector (VEC_NVME_IO_BASE + cpu)

    // Submission queue
    nvme_sqe_t*         sq;            // HHDM ptr
    phys_addr_t         sq_phys;
    uint32_t            sq_tail;
    spinlock_t          sq_lock;

    // Completion queue
    nvme_cqe_t*         cq;            // HHDM ptr
    phys_addr_t         cq_phys;
    uint32_t            cq_head;
    uint8_t             cq_phase;

    // Per-CID request slots (persistent — ISR writes into req[cqe.cid])
    nvme_request_t      req[NVME_IOQ_DEPTH];

    // Lock-free CID bitmap, per-queue.  Bit N set = CID N is free.
    volatile uint64_t   cid_free_bitmap;

    uint8_t             _pad[16];      // cache-line isolation
} __attribute__((aligned(64))) nvme_ioq_t;

static nvme_ioq_t s_ioq[MAX_CPUS];

// ── Register helpers ────────────────────────────────────────────────────
static inline uint32_t reg_rd32(uint32_t off) {
    return *(volatile uint32_t*)(s_regs + off);
}
static inline void     reg_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(s_regs + off) = v;
}
static inline uint64_t reg_rd64(uint32_t off) {
    return *(volatile uint64_t*)(s_regs + off);
}
static inline void     reg_wr64(uint32_t off, uint64_t v) {
    *(volatile uint64_t*)(s_regs + off) = v;
}

// Doorbell register for queue pair Y:
//   SQyTDBL at 0x1000 + (2*y)     * stride
//   CQyHDBL at 0x1000 + (2*y + 1) * stride
static inline volatile uint32_t* sq_doorbell(uint32_t y) {
    return (volatile uint32_t*)(s_regs + 0x1000 + (2 * y) * s_doorbell_stride);
}
static inline volatile uint32_t* cq_doorbell(uint32_t y) {
    return (volatile uint32_t*)(s_regs + 0x1000 + (2 * y + 1) * s_doorbell_stride);
}
static inline volatile uint32_t* asq_tail_doorbell(void) { return sq_doorbell(0); }
static inline volatile uint32_t* acq_head_doorbell(void) { return cq_doorbell(0); }

// Busy-wait on a 32-bit status until `(reg & mask) == want`, or timeout.
// Returns 1 on match, 0 on timeout.  Used only at bring-up time.
static uint8_t wait_for_status(uint32_t reg_off, uint32_t mask, uint32_t want,
                                uint64_t us_timeout) {
    for (uint64_t i = 0; i < us_timeout; i++) {
        if ((reg_rd32(reg_off) & mask) == want) return 1;
        // crude delay (same pattern used elsewhere in the kernel)
        for (volatile uint32_t j = 0; j < 200; j++) { }
    }
    return 0;
}

// ── Admin command submit/complete — synchronous, single outstanding ─────
// Step 1 uses the admin queue sequentially: submit, poll the CQ for the
// matching CID, return.  This keeps the bring-up code minimal.  The I/O
// path will use interrupts + wait queues.
static uint8_t admin_submit_sync(nvme_sqe_t* cmd, nvme_cqe_t* out_cqe) {
    // Build CID in the low 16 bits of the CDW0 (we reuse s_asq_tail as CID).
    uint16_t cid = (uint16_t)s_asq_tail;
    cmd->opc_fuse_psdt_cid =
        (cmd->opc_fuse_psdt_cid & 0xFFFFu) | ((uint32_t)cid << 16);

    // Copy into the ring.
    s_asq[s_asq_tail] = *cmd;

    // Advance tail and ring doorbell.
    uint32_t new_tail = (s_asq_tail + 1) % NVME_ADMIN_QDEPTH;
    __asm__ volatile("" ::: "memory");  // compiler barrier
    s_asq_tail = new_tail;
    *asq_tail_doorbell() = new_tail;

    // Poll the CQ for our CID.  QEMU's NVMe AIO backend can take many
    // milliseconds on first request while the host thread schedules —
    // use a generous timeout (~10 s worth of polls).
    volatile uint16_t* sp_ptr = (volatile uint16_t*)&s_acq[s_acq_head].status_phase;
    for (uint64_t i = 0; i < 100000000ULL; i++) {
        uint16_t sp = *sp_ptr;
        if ((sp & 1) == s_acq_phase) {
            nvme_cqe_t* cqe = &s_acq[s_acq_head];
            *out_cqe = *cqe;
            s_acq_head = (s_acq_head + 1) % NVME_ADMIN_QDEPTH;
            if (s_acq_head == 0) s_acq_phase ^= 1;
            *acq_head_doorbell() = s_acq_head;
            return (sp >> 1) == 0;  // status == 0 → success
        }
    }
    kprintf("[nvme] admin cmd timeout cid=%u opc=%x\n",
            (uint32_t)cid, cmd->opc_fuse_psdt_cid & 0xFFu);
    // Dump CQE at head — maybe device wrote it but our phase logic is off.
    nvme_cqe_t* cqe = &s_acq[s_acq_head];
    kprintf("[nvme]  CQE@%u: dw0=%x dw1=%x sq_head=%u sq_id=%u cid=%u status_phase=%x\n",
            s_acq_head, cqe->dw0, cqe->dw1, cqe->sq_head, cqe->sq_id,
            cqe->cid, cqe->status_phase);
    // Dump submitted SQE.
    nvme_sqe_t* sqe = &s_asq[(s_asq_tail + NVME_ADMIN_QDEPTH - 1) % NVME_ADMIN_QDEPTH];
    kprintf("[nvme]  SQE: opc_cid=%x nsid=%u prp1=%lx cdw10=%x\n",
            sqe->opc_fuse_psdt_cid, sqe->nsid, sqe->prp1, sqe->cdw10);
    // Dump controller status.
    kprintf("[nvme]  CSTS=%x CC=%x\n", reg_rd32(NVME_REG_CSTS), reg_rd32(NVME_REG_CC));
    return 0;
}

// ── Controller bring-up ─────────────────────────────────────────────────
static uint8_t controller_reset_and_enable(void) {
    // 1. Disable if currently enabled.
    uint32_t cc = reg_rd32(NVME_REG_CC);
    if (cc & CC_EN) {
        reg_wr32(NVME_REG_CC, cc & ~CC_EN);
    }
    if (!wait_for_status(NVME_REG_CSTS, CSTS_RDY, 0, 500000)) {
        kprintf("[nvme] timeout waiting for CSTS.RDY=0 (CSTS=%x)\n",
                reg_rd32(NVME_REG_CSTS));
        return 0;
    }

    // 2. Program AQA (admin queue sizes).
    //    [11:0]=ASQS-1, [27:16]=ACQS-1.
    uint32_t aqa = (NVME_ADMIN_QDEPTH - 1) |
                    ((NVME_ADMIN_QDEPTH - 1) << 16);
    reg_wr32(NVME_REG_AQA, aqa);
    reg_wr64(NVME_REG_ASQ, s_asq_phys);
    reg_wr64(NVME_REG_ACQ, s_acq_phys);

    // 3. Configure CC and enable.
    cc = CC_EN
       | CC_CSS_NVM
       | CC_MPS_4K
       | CC_AMS_RR
       | CC_SHN_NONE
       | CC_IOSQES_64
       | CC_IOCQES_16;
    reg_wr32(NVME_REG_CC, cc);

    if (!wait_for_status(NVME_REG_CSTS, CSTS_RDY, CSTS_RDY, 2000000)) {
        kprintf("[nvme] timeout waiting for CSTS.RDY=1 (CSTS=%x)\n",
                reg_rd32(NVME_REG_CSTS));
        return 0;
    }
    if (reg_rd32(NVME_REG_CSTS) & CSTS_CFS) {
        kprintf("[nvme] CSTS.CFS set — controller fatal status\n");
        return 0;
    }
    kprintf("[nvme] ready CC=%x CSTS=%x AQA=%x ASQ=%lx ACQ=%lx\n",
            reg_rd32(NVME_REG_CC), reg_rd32(NVME_REG_CSTS),
            reg_rd32(NVME_REG_AQA),
            (uint64_t)s_asq_phys, (uint64_t)s_acq_phys);
    return 1;
}

// ── Identify helpers ────────────────────────────────────────────────────
static uint8_t identify(uint32_t nsid, uint8_t cns, phys_addr_t buf_phys) {
    nvme_sqe_t cmd = {0};
    cmd.opc_fuse_psdt_cid = NVME_ADMIN_OP_IDENTIFY;  // opcode only; cid added by submit
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)buf_phys;
    cmd.cdw10 = cns;
    nvme_cqe_t cqe;
    return admin_submit_sync(&cmd, &cqe);
}

// ── Create I/O queue pair #1 via admin commands ─────────────────────────
// NVMe 1.4 §5.3 (Create I/O Completion Queue) and §5.4 (Create I/O
// Submission Queue).  CQ must exist before SQ can reference it.
//
// Create I/O CQ (opcode 0x05):
//   PRP1       = phys addr of CQ (4 KiB aligned, PC=1)
//   cdw10[15:0]  = QID
//   cdw10[31:16] = queue size - 1
//   cdw11[0]     = PC (physically contiguous) = 1
//   cdw11[1]     = IEN (interrupts enabled) — 0 for polling
//   cdw11[31:16] = IV (interrupt vector) — unused with IEN=0
//
// Create I/O SQ (opcode 0x01):
//   PRP1       = phys addr of SQ
//   cdw10[15:0]  = QID
//   cdw10[31:16] = queue size - 1
//   cdw11[0]     = PC = 1
//   cdw11[2:1]   = QPRIO (00 = urgent, default)
//   cdw11[31:16] = CQID (completion queue this SQ feeds into)
static uint8_t create_iocq(uint16_t qid, uint16_t qsize, phys_addr_t cq_phys,
                             uint16_t iv) {
    nvme_sqe_t cmd = {0};
    cmd.opc_fuse_psdt_cid = NVME_ADMIN_OP_CREATE_IOCQ;
    cmd.prp1 = (uint64_t)cq_phys;
    cmd.cdw10 = (uint32_t)qid | (((uint32_t)(qsize - 1)) << 16);
    // PC=1 (bit 0), IEN=1 (bit 1), IV = iv (bits 31:16)
    cmd.cdw11 = 1u | (1u << 1) | ((uint32_t)iv << 16);
    nvme_cqe_t cqe;
    return admin_submit_sync(&cmd, &cqe);
}

static uint8_t create_iosq(uint16_t qid, uint16_t qsize, phys_addr_t sq_phys,
                             uint16_t cqid) {
    nvme_sqe_t cmd = {0};
    cmd.opc_fuse_psdt_cid = NVME_ADMIN_OP_CREATE_IOSQ;
    cmd.prp1 = (uint64_t)sq_phys;
    cmd.cdw10 = (uint32_t)qid | (((uint32_t)(qsize - 1)) << 16);
    cmd.cdw11 = 1u | ((uint32_t)cqid << 16);  // PC=1, QPRIO=00, CQID
    nvme_cqe_t cqe;
    return admin_submit_sync(&cmd, &cqe);
}

// ── Free-CID allocator — per-queue lock-free bitmap ─────────────────────
// 64-bit bitmap per queue.  Bit N=1 means CID N is free.  Pop finds the
// lowest set bit and CAS-clears it.  Push atomically OR-sets.  Per-queue
// so two CPUs' allocators don't touch the same cache line.
static uint16_t cid_pop(nvme_ioq_t* q) {
    while (1) {
        uint64_t old = __atomic_load_n(&q->cid_free_bitmap, __ATOMIC_ACQUIRE);
        if (!old) {
            // Pool exhausted — only possible with >64 outstanding commands
            // on this one CPU, which means the caller is submitting faster
            // than the device completes.  Spin briefly and retry.
            for (volatile int i = 0; i < 100; i++) { }
            continue;
        }
        int bit = __builtin_ctzll(old);
        uint64_t new_val = old & ~(1ULL << bit);
        if (__atomic_compare_exchange_n(&q->cid_free_bitmap, &old, new_val, 0,
                                           __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return (uint16_t)bit;
    }
}
static void cid_push(nvme_ioq_t* q, uint16_t cid) {
    __atomic_fetch_or(&q->cid_free_bitmap, 1ULL << cid, __ATOMIC_RELEASE);
}

// PRIMITIVE (device-supplied index, category D -> delegates to index_ok).
// Is a completion's command id a valid index into the NVME_IOQ_DEPTH-entry
// req[] table?  The submit side draws cids from the 64-bit cid_free_bitmap so
// it is always in range, but the COMPLETION cid is echoed by the device and
// MUST be validated before indexing req[] -- an out-of-range cid otherwise
// drives an OOB write of req->status/->done and a wait_queue_wake_all through
// a fabricated wait_queue_t.  Pure -> unit-tested (nvme_cid_valid_selftest).
static int nvme_cid_valid(uint16_t cid) { return index_ok(cid, NVME_IOQ_DEPTH); }

// ── Async I/O submit — per-CPU queue, wakeup via that CPU's MSI-X ────
static uint8_t io_submit_async(nvme_sqe_t* cmd) {
    // Pin to the current CPU for the duration of queue selection +
    // submission.  Our scheduler has no work stealing so tasks already
    // stay on their home_cpu, but preempt_disable defends against the
    // (legal) case of a voluntary yield migrating us between picking
    // q and writing the doorbell, which would land the SQE in the
    // wrong queue's ring.
    preempt_disable();
    uint32_t cpu = this_cpu()->id;
    if (cpu >= s_nr_ioq) cpu = 0;   // fallback — shouldn't happen post-init
    nvme_ioq_t* q = &s_ioq[cpu];

    uint16_t cid = cid_pop(q);
    nvme_request_t* req = &q->req[cid];
    wait_queue_init(&req->wq);
    __atomic_store_n(&req->done, 0u, __ATOMIC_RELEASE);
    req->status = 0;

    cmd->opc_fuse_psdt_cid =
        (cmd->opc_fuse_psdt_cid & 0xFFFFu) | ((uint32_t)cid << 16);

    // sq_lock defends against same-CPU preemption between two submitters
    // on the same queue.  It's taken with IRQs disabled so the owning
    // CPU's ISR can't deadlock on it while waiting for us to release.
    uint64_t flags = spin_lock_irqsave(&q->sq_lock);
    uint32_t tail = q->sq_tail;
    q->sq[tail] = *cmd;
    uint32_t new_tail = (tail + 1) % NVME_IOQ_DEPTH;
    q->sq_tail = new_tail;
    // SFENCE drains the store buffer so the device's DMA-read of the
    // SQE observes the full 64 bytes before it sees the new tail via
    // the doorbell MMIO write.
    __asm__ volatile("sfence" ::: "memory");
    *sq_doorbell(q->qid) = new_tail;
    spin_unlock_irqrestore(&q->sq_lock, flags);

    // Re-enable preemption before WAIT_EVENT — sched_sleep requires
    // preempt_depth==0 (asserts otherwise).  preempt_enable() is safe
    // here: we're in task context, not an ISR, so a trailing
    // sched_preempt → do_switch → sti is just a normal voluntary yield.
    preempt_enable();

    WAIT_EVENT(&req->wq, __atomic_load_n(&req->done, __ATOMIC_ACQUIRE));
    __asm__ volatile("mfence" ::: "memory");
    uint16_t status = req->status;
    cid_push(q, cid);
    return (status >> 1) == 0;
}

// ── I/O completion IRQ handler ──────────────────────────────────────────
// Drains THIS CPU's completion queue.  MSI-X steers vector
// VEC_NVME_IO_BASE + cpu_id only to cpu_id, so this handler runs on
// the CPU that owns q->cpu == this_cpu()->id — no cross-CPU state is
// touched and no serialisation is needed for cq_head / cq_phase.
//
// preempt_disable is still required because the body calls
// wait_queue_wake_all → rcu_read_unlock → preempt_enable, which at
// depth==0 with reschedule_pending set would do_switch → trailing `sti`,
// re-enabling IRQs inside this handler and letting the next MSI-X
// nest on top.  Staying at depth>=1 short-circuits that path; any
// pending reschedule is picked up after iretq.
void nvme_irq_handler(void) {
    uint32_t cpu = this_cpu()->id;
    if (cpu >= s_nr_ioq) return;        // IRQ before per-CPU queues ready
    nvme_ioq_t* q = &s_ioq[cpu];
    if (!q->cq) return;

    preempt_disable();
    while (1) {
        volatile uint16_t* sp_ptr =
            (volatile uint16_t*)&q->cq[q->cq_head].status_phase;
        uint16_t sp = *sp_ptr;
        if ((sp & 1) != q->cq_phase) break;
        nvme_cqe_t cqe = q->cq[q->cq_head];
        q->cq_head = (q->cq_head + 1) % NVME_IOQ_DEPTH;
        if (q->cq_head == 0) q->cq_phase ^= 1;

        // cqe.cid is echoed by the device; reject an out-of-range id before it
        // indexes req[] (OOB write + wake through a garbage wait_queue_t).  The
        // CQE is already consumed (cq_head/phase advanced), so the doorbell
        // below still credits it -- we just drop the corrupt completion.
        if (!nvme_cid_valid(cqe.cid)) continue;

        nvme_request_t* req = &q->req[cqe.cid];
        req->status = cqe.status_phase;
        // Full fence so the device's preceding DMA to the caller buffer
        // is globally ordered before the waker observes done=1.
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        __atomic_store_n(&req->done, 1u, __ATOMIC_RELEASE);
        wait_queue_wake_all(&req->wq);
    }
    *cq_doorbell(q->qid) = q->cq_head;
    // Manual decrement — calling preempt_enable() here would itself
    // re-open the sched_preempt → do_switch → `sti` path that would
    // let the next MSI-X nest inside this handler.
    preempt_enable_no_resched();
}

// Expose asm stub.
extern void nvme_irq_entry(void);

// Convert a kernel virtual address to a physical address.  We only
// support HHDM pointers here (that's all the block layer feeds us).
static inline phys_addr_t kva_to_phys(const void* va) {
    uint64_t v = (uint64_t)va;
    return (phys_addr_t)(v - HHDM_OFFSET);
}

// Submit a READ or WRITE against namespace s_ns_nsid.
//
// PRP layout for NVMe 1.4 §4.4:
//   - PRP1 = phys addr of first buffer page (can have page-byte offset)
//   - PRP2:
//       * 0 if total transfer fits in PRP1 alone (≤ (4K - prp1_off) bytes)
//       * phys addr of second 4K page if total ≤ 8K
//       * phys addr of PRP list page if total > 8K
// For the step-2 smoke path we limit transfers to ≤ 8 KiB and only
// populate PRP1/PRP2 directly.  The later block layer will build PRP
// lists for larger I/Os.
static uint8_t nvme_rw(uint64_t lba, void* buf, uint32_t nlb, uint8_t write) {
    if (!nlb) return 1;
    // The 2-PRP path supports <= 8 KiB.  Form the byte length in u64 (mul_within_u32)
    // so a huge nlb cannot wrap u32 and slip past this bound while cdw12 still carries
    // a (16-bit-masked) large sector count -> the device DMAs far past the PRPs.
    uint32_t bytes;
    if (!mul_within_u32(nlb, s_ns_lba_size, 8192u, &bytes)) {
        kprintf("[nvme] nvme_rw: >8KB transfers not yet supported\n");
        return 0;
    }
    phys_addr_t pa = kva_to_phys(buf);
    uint32_t pg_off = (uint32_t)(pa & 0xFFFu);
    phys_addr_t prp1 = pa;
    phys_addr_t prp2 = 0;
    uint32_t first_chunk = 4096u - pg_off;
    if (bytes > first_chunk) {
        if (bytes - first_chunk > 4096u) {
            kprintf("[nvme] nvme_rw: >8KB transfers not yet supported\n");
            return 0;
        }
        prp2 = (pa & ~0xFFFULL) + 0x1000ULL;
    }

    nvme_sqe_t cmd = {0};
    cmd.opc_fuse_psdt_cid = write ? NVME_NVM_OP_WRITE : NVME_NVM_OP_READ;
    cmd.nsid = s_ns_nsid;
    cmd.prp1 = (uint64_t)prp1;
    cmd.prp2 = (uint64_t)prp2;
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (uint32_t)(nlb - 1) & 0xFFFFu;  // NLB is 0-based
    return io_submit_async(&cmd);
}

uint8_t nvme_read (uint64_t lba, void*       buf, uint32_t nlb) {
    return nvme_rw(lba, buf, nlb, 0);
}
uint8_t nvme_write(uint64_t lba, const void* buf, uint32_t nlb) {
    return nvme_rw(lba, (void*)buf, nlb, 1);
}

// ── MSI-X setup — one entry per CPU, each steered to its LAPIC ──────────
// Locate the MSI-X capability, map as many table entries as we need,
// program each entry `cpu` with (MSI addr = LAPIC-of-cpu, data = vector
// VEC_NVME_IO_BASE + cpu), then enable MSI-X.  Every per-CPU I/O queue
// uses its own IV (== cpu), so completions fire on that CPU only.
// Returns 0 on failure; on success, `*out_tbl_size` gets the usable
// entry count (for the caller to cap s_nr_ioq).
static uint8_t nvme_msix_enable(uint32_t nr_wanted, uint32_t* out_tbl_size) {
    uint8_t msix_cap = pci_find_cap(s_pci.bus, s_pci.dev, s_pci.fn, 0x11u);
    if (!msix_cap) {
        kprintf("[nvme] no MSI-X capability\n");
        return 0;
    }
    uint32_t mc0 = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn, msix_cap);
    uint32_t tbl_size = ((mc0 >> 16) & 0x7FFu) + 1u;   // bits 26:16 (0-based)
    if (tbl_size < nr_wanted) {
        kprintf("[nvme] MSI-X table has %u entries, wanted %u — capping\n",
                tbl_size, nr_wanted);
        nr_wanted = tbl_size;
    }

    uint32_t tbl_dw = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn,
                                       msix_cap + 4u);
    uint32_t bir     = tbl_dw & 0x7u;
    uint32_t tbl_off = tbl_dw & ~0x7u;
    uint64_t bar_phys = pci_bar_base(s_pci.bus, s_pci.dev, s_pci.fn,
                                      (uint8_t)bir);
    if (!bar_phys) { kprintf("[nvme] MSI-X BAR%u is zero\n", bir); return 0; }

    // Map enough space for all entries we intend to program.
    uint32_t map_bytes = 16u * tbl_size;
    // Round up to page so vmm_map_mmio is happy.
    map_bytes = (uint32_t)page_align_up(map_bytes);
    s_msix_table = (volatile uint32_t*)
                   vmm_map_mmio(bar_phys + tbl_off, map_bytes);
    if (!s_msix_table) {
        // MSI-X is required here (each per-CPU I/O queue uses its own vector;
        // this driver has no INTx fallback), so a failed table map fails enable
        // -- consistent with the !bar_phys guard above -- rather than NULL-deref
        // the programming loop below.
        kprintf("[nvme] MSI-X table map failed\n");
        return 0;
    }

    // Program one entry per CPU we actually want to service.
    for (uint32_t cpu = 0; cpu < nr_wanted; cpu++) {
        uint8_t vec = (uint8_t)(VEC_NVME_IO_BASE + cpu);
        // MSI address format for physical-mode fixed delivery:
        //   0xFEE00000 | (dest_apic_id << 12)
        // lapic_msi_addr() returns it for the *current* CPU; we need
        // it per-destination, so build it inline.
        uint32_t apic_id = g_acpi.ok
                            ? (g_acpi.cpus[cpu].apic_id & 0xFFu)
                            : (uint32_t)cpu;
        uint32_t msi_addr = 0xFEE00000u | (apic_id << 12);
        s_msix_table[cpu * 4 + 0] = msi_addr;    // addr_lo
        s_msix_table[cpu * 4 + 1] = 0;            // addr_hi
        s_msix_table[cpu * 4 + 2] = (uint32_t)vec; // data = vector
        s_msix_table[cpu * 4 + 3] = 0;            // vector_ctrl: unmask

        idt_irq_register(vec, (uint64_t)nvme_irq_entry);
    }
    // Mask any remaining table entries (device may have > nr_wanted).
    for (uint32_t i = nr_wanted; i < tbl_size; i++) {
        s_msix_table[i * 4 + 3] = 1u;   // masked
    }

    // Enable MSI-X (bit 31), clear Function Mask (bit 30).
    uint32_t mc = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn, msix_cap);
    mc = (mc | (1u << 31)) & ~(1u << 30);
    pci_cfg_write32(s_pci.bus, s_pci.dev, s_pci.fn, msix_cap, mc);

    if (out_tbl_size) *out_tbl_size = tbl_size;
    return 1;
}

// ── Public entry ────────────────────────────────────────────────────────
uint8_t nvme_init(void) {
    // 1. PCI discovery.  NVMe = class 0x01, subclass 0x08, prog-if 0x02.
    pci_device_t dev;
    if (!pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, &dev)) {
        // Not present — that's fine, AHCI path is still active.
        return 0;
    }
    if (dev.prog_if != PCI_PROG_IF_NVME) {
        kprintf("[nvme] PCI device at %u:%u.%u has unexpected prog_if=%x\n",
                dev.bus, dev.dev, dev.fn, dev.prog_if);
        return 0;
    }

    s_pci = dev;
    kprintf("[nvme] found controller %x:%x at %u:%u.%u\n",
            dev.vendor_id, dev.device_id, dev.bus, dev.dev, dev.fn);

    // 2. Enable MMIO + bus master, map BAR0 (8 KiB is enough for CAP+doorbells).
    pci_enable(dev.bus, dev.dev, dev.fn);
    uint64_t bar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn, 0);
    if (!bar_phys) {
        kprintf("[nvme] BAR0 is zero\n");
        return 0;
    }
    s_regs = (uint8_t*)vmm_map_mmio(bar_phys, 0x2000);
    // A failed BAR0 map (OOM / MMIO-VA exhaustion -> NULL) must fail init, not
    // NULL-deref s_regs in reg_rd32 below.  nvme_init returns 0 on every other
    // setup failure; mirror that.
    if (!s_regs) return 0;

    // 3. Read CAP to learn doorbell stride and max queue depth.  Read as
    //    two 32-bit halves — some controllers don't support a 64-bit MMIO
    //    read of CAP (or the compiler may split it anyway).
    uint32_t cap_lo = reg_rd32(NVME_REG_CAP);
    uint32_t cap_hi = reg_rd32(NVME_REG_CAP + 4);
    s_cap = ((uint64_t)cap_hi << 32) | cap_lo;
    uint32_t dstrd = (uint32_t)((s_cap >> 32) & 0xF);   // CAP.DSTRD
    uint32_t mqes  = (uint32_t)(s_cap & 0xFFFF);         // CAP.MQES
    s_doorbell_stride = 4u << dstrd;
    s_max_qentries = mqes + 1;

    uint32_t vs = reg_rd32(NVME_REG_VS);
    kprintf("[nvme] VS=%x.%x.%x CAP_lo=%x CAP_hi=%x mqes=%u dstrd_field=%u stride=%u\n",
            (vs >> 16) & 0xFFFF, (vs >> 8) & 0xFF, vs & 0xFF,
            cap_lo, cap_hi, s_max_qentries, dstrd, s_doorbell_stride);

    // Size the BAR0 mapping to cover the doorbells of every I/O queue we may
    // create.  The highest doorbell is cq_doorbell(MAX_CPUS) = 0x1000 +
    // (2*MAX_CPUS + 1)*stride; the stride is the device-reported 4 << CAP.DSTRD,
    // so the fixed 0x2000 map above is overrun once DSTRD is large enough -- an
    // OOB MMIO store at the sq/cq_doorbell write sites.  Remap over the real
    // extent before any doorbell is touched (the admin queue setup below writes
    // the qid-0 doorbells).  The common small-stride case (QEMU/real devices,
    // DSTRD=0 -> extent 0x1208) stays within 0x2000 and is left unchanged.
    uint64_t db_extent = 0x1000ull + (2ull * MAX_CPUS + 1ull) * s_doorbell_stride + 4ull;
    db_extent = page_align_up(db_extent);             // round up to a page
    if (db_extent > NVME_BAR0_MAX_MAP) {
        kprintf("[nvme] CAP.DSTRD=%u needs a %u-byte doorbell region (> %u); refusing\n",
                dstrd, (uint32_t)db_extent, (uint32_t)NVME_BAR0_MAX_MAP);
        return 0;
    }
    if (db_extent > 0x2000ull) {
        s_regs = (uint8_t*)vmm_map_mmio(bar_phys, db_extent);
        // The remap to the larger doorbell window replaced the first mapping;
        // on failure s_regs is now NULL, so fail init rather than NULL-deref
        // the doorbells.
        if (!s_regs) return 0;
    }

    // 4. Allocate admin queues (one 4 KiB page each — enough for 64 entries
    //    of SQE=64B (4096/64=64) and CQE=16B (4096/16=256)).
    s_asq_phys = pmm_buddy_alloc(0);
    s_acq_phys = pmm_buddy_alloc(0);
    if (s_asq_phys == PMM_INVALID_ADDR || s_acq_phys == PMM_INVALID_ADDR) {
        kprintf("[nvme] PMM OOM\n");
        return 0;
    }
    s_asq = (nvme_sqe_t*)(s_asq_phys + HHDM_OFFSET);
    s_acq = (nvme_cqe_t*)(s_acq_phys + HHDM_OFFSET);
    __builtin_memset(s_asq, 0, 4096);
    __builtin_memset(s_acq, 0, 4096);
    s_asq_tail = 0;
    s_acq_head = 0;
    s_acq_phase = 1;

    // 5. Reset + enable.
    if (!controller_reset_and_enable()) return 0;

    // 6. Identify Controller — 4 KiB buffer.
    phys_addr_t idbuf_phys = pmm_buddy_alloc(0);
    if (idbuf_phys == PMM_INVALID_ADDR) return 0;
    uint8_t* idbuf = (uint8_t*)(idbuf_phys + HHDM_OFFSET);
    __builtin_memset(idbuf, 0, 4096);

    if (!identify(0, NVME_IDENT_CNS_CTRL, idbuf_phys)) {
        kprintf("[nvme] Identify Controller failed\n");
        return 0;
    }
    // Vendor ID at offset 0, Model Number at offset 24 (40 chars, space-padded).
    uint16_t vid = *(uint16_t*)(idbuf + 0);
    uint16_t ssvid = *(uint16_t*)(idbuf + 2);
    char serial[21] = {0};
    __builtin_memcpy(serial, idbuf + 4, 20);
    char model[41] = {0};
    __builtin_memcpy(model, idbuf + 24, 40);
    // Trim trailing spaces.
    for (int i = 19; i >= 0 && serial[i] == ' '; i--) serial[i] = 0;
    for (int i = 39; i >= 0 && model[i] == ' ';  i--) model[i]  = 0;
    kprintf("[nvme] Identify Controller: vid=%x ssvid=%x model='%s' sn='%s'\n",
            (uint32_t)vid, (uint32_t)ssvid, model, serial);

    // 7. Identify active namespaces (CNS=0x02 returns up to 1024 NSIDs).
    if (!identify(0, NVME_IDENT_CNS_NS_LIST, idbuf_phys)) {
        kprintf("[nvme] Identify Namespace List failed\n");
        return 0;
    }
    uint32_t first_nsid = *(uint32_t*)idbuf;
    kprintf("[nvme] first active NSID = %u\n", first_nsid);
    if (!first_nsid) return 0;

    // 8. Identify first namespace — proves per-NS commands work.
    if (!identify(first_nsid, NVME_IDENT_CNS_NS, idbuf_phys)) {
        kprintf("[nvme] Identify Namespace %u failed\n", first_nsid);
        return 0;
    }
    uint64_t nsze = *(uint64_t*)(idbuf + 0);
    uint8_t  flbas = idbuf[26];
    uint8_t  lbaf_i = flbas & 0xF;
    uint32_t lbaf  = *(uint32_t*)(idbuf + 128 + 4 * lbaf_i);
    uint32_t lbads = (lbaf >> 16) & 0xFF;   // 2^lbads = LBA size (device-supplied)
    uint32_t lba_size;
    if (!nvme_lba_size_ok(lbads, &lba_size)) {
        // Out-of-range / UB-inducing LBADS: refuse the namespace rather than
        // derive a bogus sector size that defeats nvme_rw's DMA-size guard.
        kprintf("[nvme] NS%u: unsupported LBA data size 2^%u -- refusing namespace\n",
                first_nsid, lbads);
        return 0;
    }
    s_ns_nsid = first_nsid;
    s_ns_lba_size = lba_size;
    kprintf("[nvme] NS%u: nsze=%lu lba_size=%u bytes\n",
            first_nsid, nsze, s_ns_lba_size);

    // 9. Create one I/O queue pair per CPU.  ACPI gives us the CPU count
    //    even before APs are brought up (g_num_cpus still == 1 at this
    //    point in the init chain).  Each queue i owns CPU i, its MSI-X
    //    vector is VEC_NVME_IO_BASE+i, and its MSI destination is that
    //    CPU's LAPIC ID — so completions fire only on the owning CPU.
    uint32_t nr_cpus = (g_acpi.ok && g_acpi.cpu_count)
                        ? g_acpi.cpu_count : 1u;
    if (nr_cpus > MAX_CPUS) nr_cpus = MAX_CPUS;

    // 9a. Program MSI-X for all N vectors BEFORE any Create-CQ admin
    //     command (which arms interrupts with the given IV).  The helper
    //     reads back the device's MSI-X table size and may cap nr_cpus.
    uint32_t msix_tbl_size = 0;
    if (!nvme_msix_enable(nr_cpus, &msix_tbl_size)) return 0;
    uint32_t nr_queues = nr_cpus < msix_tbl_size ? nr_cpus : msix_tbl_size;

    // 9b. Build per-CPU queue state and issue Create-IOCQ / Create-IOSQ
    //     admin commands for each.
    for (uint32_t cpu = 0; cpu < nr_queues; cpu++) {
        nvme_ioq_t* q = &s_ioq[cpu];
        phys_addr_t sq_phys = pmm_buddy_alloc(0);
        phys_addr_t cq_phys = pmm_buddy_alloc(0);
        if (sq_phys == PMM_INVALID_ADDR || cq_phys == PMM_INVALID_ADDR) {
            kprintf("[nvme] OOM allocating I/O queue %u\n", cpu);
            return 0;
        }
        __builtin_memset((void*)(sq_phys + HHDM_OFFSET), 0, 4096);
        __builtin_memset((void*)(cq_phys + HHDM_OFFSET), 0, 4096);

        q->qid       = NVME_IOQ_QID(cpu);
        q->cpu       = (uint16_t)cpu;
        q->msix_vec  = (uint8_t)(VEC_NVME_IO_BASE + cpu);
        q->sq        = (nvme_sqe_t*)(sq_phys + HHDM_OFFSET);
        q->sq_phys   = sq_phys;
        q->sq_tail   = 0;
        q->cq        = (nvme_cqe_t*)(cq_phys + HHDM_OFFSET);
        q->cq_phys   = cq_phys;
        q->cq_head   = 0;
        q->cq_phase  = 1;
        q->cid_free_bitmap = (NVME_IOQ_DEPTH == 64)
                               ? (uint64_t)-1
                               : ((1ULL << NVME_IOQ_DEPTH) - 1ULL);
        spin_lock_init(&q->sq_lock);

        // CQ first (with IV=cpu), then SQ.
        if (!create_iocq(q->qid, NVME_IOQ_DEPTH, cq_phys, (uint16_t)cpu)) {
            kprintf("[nvme] Create I/O CQ #%u failed\n", q->qid);
            return 0;
        }
        if (!create_iosq(q->qid, NVME_IOQ_DEPTH, sq_phys, q->qid)) {
            kprintf("[nvme] Create I/O SQ #%u failed\n", q->qid);
            return 0;
        }
    }
    s_nr_ioq = nr_queues;
    kprintf("[nvme] %u I/O queue pairs created (one per CPU, depth=%u)\n",
            s_nr_ioq, (uint32_t)NVME_IOQ_DEPTH);

    // 10. Smoke test from the BSP (only CPU online at this point).
    phys_addr_t t_phys = pmm_buddy_alloc(0);
    if (t_phys != PMM_INVALID_ADDR) {
        uint8_t* t = (uint8_t*)(t_phys + HHDM_OFFSET);
        __builtin_memset(t, 0xCC, 4096);  // poison — overwritten by DMA
        if (nvme_read(0, t, 8)) {
            kprintf("[nvme] smoke read LBA0: "
                    "%x %x %x %x %x %x %x %x | %x %x %x %x %x %x %x %x\n",
                    t[0],  t[1],  t[2],  t[3],  t[4],  t[5],  t[6],  t[7],
                    t[8],  t[9],  t[10], t[11], t[12], t[13], t[14], t[15]);
        } else {
            kprintf("[nvme] smoke read LBA0 FAILED\n");
        }
    }

    kprintf("[nvme] init complete — per-CPU I/O queues live\n");
    return 1;
}

// ── nvme_cid_valid selftest ───────────────────────────────────────────────
// Deterministic check of the device-echoed completion-id bounds guard that
// stops an OOB write of req[]/a wake through a fabricated wait_queue_t from a
// malicious/buggy controller (or CQ-page DMA corruption) reporting cid >= 64.
void nvme_cid_valid_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint16_t cid; int want; } c[] = {
        { 0,                 1 },  // first slot
        { NVME_IOQ_DEPTH-1,  1 },  // last valid slot (63)
        { NVME_IOQ_DEPTH,    0 },  // == 64, first OOB
        { 1000,              0 },  // arbitrary OOB
        { 0xFFFF,            0 },  // device garbage (uint16 max)
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        if (nvme_cid_valid(c[i].cid) != c[i].want) {
            kprintf_atomic("[nvme_cid] FAIL cid=%u got=%d want=%d\n",
                    (unsigned)c[i].cid, nvme_cid_valid(c[i].cid), c[i].want);
            fails++;
        }
    }
    kprintf_atomic(fails ? "[nvme_cid] SELF-TEST FAILED\n"
                  : "[nvme_cid] SELF-TEST PASSED (device completion-id bounds)\n");
}

// Deterministic check of the LBADS validation: only the driver-supported
// 512..4096-byte sizes (lbads 9..12) are accepted, the sub-512 / oversized
// values are rejected, and the lbads >= 32 rows are exactly the `1u << lbads`
// UB the raw code hit (the shift count wraps to & 31, yielding a tiny size that
// defeats the DMA-size guard).  Rejecting BEFORE the shift makes the UB
// unreachable.
void nvme_lba_size_ok_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint32_t lbads; bool eok; uint32_t esize; } c[] = {
        { 9,   true,  512 },    // 512-byte sector (the common case)
        { 10,  true,  1024 },
        { 11,  true,  2048 },
        { 12,  true,  4096 },   // 4 KiB sector
        { 8,   false, 0 },      // 256-byte: sub-512, reject
        { 0,   false, 0 },      // 1-byte: the guard-defeat input, reject
        { 13,  false, 0 },      // 8 KiB: > 2-PRP path / oversized, reject
        { 31,  false, 0 },      // 2^31: no UB but huge, reject
        { 32,  false, 0 },      // THE UB edge: 1u<<32 is undefined -> rejected before the shift
        { 255, false, 0 },      // max 8-bit field, reject
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        uint32_t sz = 0xDEADu;
        bool ok = nvme_lba_size_ok(c[i].lbads, &sz);
        if (ok != c[i].eok || (ok && sz != c[i].esize)) {
            kprintf_atomic("[nvme_lbads] FAIL lbads=%u ok=%d sz=%u\n",
                    c[i].lbads, (int)ok, sz);
            fails++;
        }
    }
    kprintf_atomic(fails ? "[nvme_lbads] SELF-TEST FAILED\n"
                  : "[nvme_lbads] SELF-TEST PASSED (valid 512..4096 LBA size, no UB shift)\n");
}
