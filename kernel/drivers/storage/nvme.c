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
// We use 64 entries for both — matches admin depth, plenty for a smoke
// test.
#define NVME_IOQ_DEPTH    64
#define NVME_IOQ_ID       1u

static nvme_sqe_t*     s_iosq = NULL;
static nvme_cqe_t*     s_iocq = NULL;
static phys_addr_t     s_iosq_phys = 0;
static phys_addr_t     s_iocq_phys = 0;
static uint32_t        s_iosq_tail = 0;
static uint32_t        s_iocq_head = 0;
static uint8_t         s_iocq_phase = 1;

static uint32_t        s_ns_nsid = 0;      // first namespace id
static uint32_t        s_ns_lba_size = 512;

// PCI location — saved so MSI-X setup can write PCI config.
static pci_device_t    s_pci;
static volatile uint32_t* s_msix_table = NULL;  // 16 B per entry × N entries

// Per-request state.  Submitter stack-allocates one.  The persistent
// sleep_we in task_t (used by WAIT_EVENT) protects us from stack-UAF
// if a late wake arrives after our frame pops — req->wq is only held
// for the duration of WAIT_EVENT, which blocks until req->done=1.
typedef struct nvme_request {
    wait_queue_t      wq;
    volatile uint8_t  done;
    volatile uint16_t status;
} nvme_request_t;

// Persistent per-CID request slots.  NOT stack-allocated — a late
// wake from the ISR (after the submitter's frame has popped via
// wake_pending-driven early return) would otherwise clobber freed
// stack memory.  CID-indexed so the ISR finds the slot directly from
// the CQE's CID field.
static nvme_request_t    s_req[NVME_IOQ_DEPTH] __attribute__((aligned(64)));

// CID allocator: 64-bit free bitmap, one bit per CID.  Pop = find lowest
// set bit + CAS it off.  Push = atomic_fetch_or.  Lock-free; immune to
// ABA because we never write the backing store ahead of the commit.
static volatile uint64_t s_cid_free_bitmap;

// Tail-advance lock.  Multiple submitters can race to write the SQE
// and advance the tail; serialize that little slice so the SQE at
// sq_tail is ours and the doorbell write matches.  Held for a handful
// of stores — no contention cost under normal load.
static spinlock_t               s_iosq_lock;

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

// ── Free-CID allocator — lock-free bitmap ───────────────────────────────
// 64-bit bitmap, bit N=1 means CID N is free.  Pop finds the lowest set
// bit and CAS-clears it.  Push atomically OR-sets the bit.  No backing
// store updated ahead of the commit, so no race window.
static uint16_t cid_pop(void) {
    while (1) {
        uint64_t old = __atomic_load_n(&s_cid_free_bitmap, __ATOMIC_ACQUIRE);
        if (!old) {
            // Pool exhausted.  Shouldn't happen at step-3 scale; spin
            // briefly and retry.  (Later: wait on a slot-available wq.)
            for (volatile int i = 0; i < 100; i++) { }
            continue;
        }
        int bit = __builtin_ctzll(old);
        uint64_t new_val = old & ~(1ULL << bit);
        if (__atomic_compare_exchange_n(&s_cid_free_bitmap, &old, new_val, 0,
                                           __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return (uint16_t)bit;
    }
}
static void cid_push(uint16_t cid) {
    __atomic_fetch_or(&s_cid_free_bitmap, 1ULL << cid, __ATOMIC_RELEASE);
}

// ── Async I/O submit — wakeup via MSI-X ISR ─────────────────────────────
static uint8_t io_submit_async(nvme_sqe_t* cmd) {
    uint16_t cid = cid_pop();
    nvme_request_t* req = &s_req[cid];
    wait_queue_init(&req->wq);
    __atomic_store_n(&req->done, 0u, __ATOMIC_RELEASE);
    req->status = 0;

    cmd->opc_fuse_psdt_cid =
        (cmd->opc_fuse_psdt_cid & 0xFFFFu) | ((uint32_t)cid << 16);

    // Serialize the SQE store + tail advance + doorbell write.
    uint64_t flags = spin_lock_irqsave(&s_iosq_lock);
    uint32_t tail = s_iosq_tail;
    s_iosq[tail] = *cmd;
    uint32_t new_tail = (tail + 1) % NVME_IOQ_DEPTH;
    s_iosq_tail = new_tail;
    __asm__ volatile("" ::: "memory");
    *sq_doorbell(NVME_IOQ_ID) = new_tail;  // MMIO: serializing write
    spin_unlock_irqrestore(&s_iosq_lock, flags);

    WAIT_EVENT(&req->wq, __atomic_load_n(&req->done, __ATOMIC_ACQUIRE));
    uint16_t status = req->status;
    cid_push(cid);
    return (status >> 1) == 0;
}

// ── I/O completion IRQ handler ──────────────────────────────────────────
// Drains the CQ: for each new entry, look up the request via CID and
// wake it with the reported status.  Per-request wait queue means at
// most one waiter ever — wake_all is O(1).
void nvme_irq_handler(void) {
    if (!s_iocq) return;
    while (1) {
        volatile uint16_t* sp_ptr =
            (volatile uint16_t*)&s_iocq[s_iocq_head].status_phase;
        uint16_t sp = *sp_ptr;
        if ((sp & 1) != s_iocq_phase) break;
        nvme_cqe_t cqe = s_iocq[s_iocq_head];
        s_iocq_head = (s_iocq_head + 1) % NVME_IOQ_DEPTH;
        if (s_iocq_head == 0) s_iocq_phase ^= 1;

        // Persistent per-CID slot — no lookup, no xchg, no UAF risk.
        nvme_request_t* req = &s_req[cqe.cid];
        req->status = cqe.status_phase;
        __atomic_store_n(&req->done, 1u, __ATOMIC_RELEASE);
        wait_queue_wake_all(&req->wq);
    }
    // Acknowledge consumed CQEs — ring the CQ head doorbell.
    *cq_doorbell(NVME_IOQ_ID) = s_iocq_head;
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
    uint32_t bytes = nlb * s_ns_lba_size;
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

// ── MSI-X setup — program vector 0 to deliver VEC_NVME_IO to BSP ────────
// Follows the same pattern as ahci.c: find the MSI-X capability, map
// the MSI-X table BAR, program entry 0 with our LAPIC MSI address/data,
// then unmask MSI-X in PCI config.  Fixed-delivery to BSP works in both
// xAPIC and x2APIC modes.  Per-CPU steering lands in step 4.
static uint8_t nvme_msix_enable(void) {
    uint8_t cap = (uint8_t)(pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn,
                                              0x34u) & 0xFCu);
    uint8_t msix_cap = 0;
    while (cap) {
        uint32_t dw = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn, cap);
        if ((dw & 0xFF) == 0x11u) { msix_cap = cap; break; }
        cap = (uint8_t)((dw >> 8) & 0xFCu);
    }
    if (!msix_cap) {
        kprintf("[nvme] no MSI-X capability\n");
        return 0;
    }
    uint32_t tbl_dw = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn,
                                       msix_cap + 4u);
    uint32_t bir     = tbl_dw & 0x7u;
    uint32_t tbl_off = tbl_dw & ~0x7u;
    uint64_t bar_phys = pci_bar_base(s_pci.bus, s_pci.dev, s_pci.fn,
                                      (uint8_t)bir);
    if (!bar_phys) { kprintf("[nvme] MSI-X BAR%u is zero\n", bir); return 0; }

    // Map 16 B × 1 entry (we only use vector 0 at step 3).
    s_msix_table = (volatile uint32_t*)
                   vmm_map_mmio(bar_phys + tbl_off, 16u);

    uint32_t msi_addr = (uint32_t)lapic_msi_addr();
    uint32_t msi_data = lapic_msi_data(VEC_NVME_IO);
    s_msix_table[0] = msi_addr;      // addr_lo
    s_msix_table[1] = 0;              // addr_hi
    s_msix_table[2] = msi_data;       // data
    s_msix_table[3] = 0;              // vector_ctrl: unmask

    // Enable MSI-X in message control (bit 31), clear Function Mask (bit 30).
    uint32_t mc = pci_cfg_read32(s_pci.bus, s_pci.dev, s_pci.fn, msix_cap);
    mc = (mc | (1u << 31)) & ~(1u << 30);
    pci_cfg_write32(s_pci.bus, s_pci.dev, s_pci.fn, msix_cap, mc);

    idt_irq_register(VEC_NVME_IO, (uint64_t)nvme_irq_entry);
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
    uint32_t lbads = (lbaf >> 16) & 0xFF;   // 2^lbads = LBA size
    s_ns_nsid = first_nsid;
    s_ns_lba_size = 1u << lbads;
    kprintf("[nvme] NS%u: nsze=%lu lba_size=%u bytes\n",
            first_nsid, nsze, s_ns_lba_size);

    // 9. Create one I/O queue pair (step-2 scope — single queue, polling).
    s_iosq_phys = pmm_buddy_alloc(0);
    s_iocq_phys = pmm_buddy_alloc(0);
    if (s_iosq_phys == PMM_INVALID_ADDR || s_iocq_phys == PMM_INVALID_ADDR) {
        kprintf("[nvme] OOM allocating I/O queue pages\n");
        return 0;
    }
    s_iosq = (nvme_sqe_t*)(s_iosq_phys + HHDM_OFFSET);
    s_iocq = (nvme_cqe_t*)(s_iocq_phys + HHDM_OFFSET);
    __builtin_memset(s_iosq, 0, 4096);
    __builtin_memset(s_iocq, 0, 4096);

    // 9a. MSI-X must be programmed BEFORE Create I/O CQ, because CQ
    //     creation arms interrupts with the given IV.
    if (!nvme_msix_enable()) return 0;

    // 9b. Initialize per-request plumbing.
    spin_lock_init(&s_iosq_lock);
    // Mark CIDs 0..NVME_IOQ_DEPTH-1 as free (bits 0..depth-1 set).
    s_cid_free_bitmap = (NVME_IOQ_DEPTH == 64)
                         ? (uint64_t)-1
                         : ((1ULL << NVME_IOQ_DEPTH) - 1ULL);

    // 9c. Create CQ (IEN=1, IV=0), then SQ.
    if (!create_iocq(NVME_IOQ_ID, NVME_IOQ_DEPTH, s_iocq_phys, 0)) {
        kprintf("[nvme] Create I/O CQ failed\n");
        return 0;
    }
    if (!create_iosq(NVME_IOQ_ID, NVME_IOQ_DEPTH, s_iosq_phys, NVME_IOQ_ID)) {
        kprintf("[nvme] Create I/O SQ failed\n");
        return 0;
    }
    kprintf("[nvme] I/O queue pair #%u created (depth=%u, IEN=1 IV=0)\n",
            (uint32_t)NVME_IOQ_ID, (uint32_t)NVME_IOQ_DEPTH);

    // 10. Smoke test: read LBA 0 into a fresh 4 KiB page and dump the
    //     first 32 bytes.  This proves the full submit→complete path.
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

    kprintf("[nvme] step 2 complete — I/O queue live\n");
    return 1;
}
