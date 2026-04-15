#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "common.h"
#include "idt.h"
#include "lapic.h"
#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "wait.h"
#include "smp.h"

// ── PCI class codes for AHCI ──────────────────────────────────────────────
#define PCI_CLASS_STORAGE  0x01
#define PCI_SUBCLASS_AHCI  0x06
#define AHCI_BAR           5       // ABAR = BAR5

// ── HBA register offsets / bits ───────────────────────────────────────────
// GHC register
#define HBA_GHC_AE   (1u << 31)   // AHCI Enable
#define HBA_GHC_IE   (1u << 1)    // Interrupt Enable (global)
#define HBA_GHC_HR   (1u << 0)    // HBA Reset

// Port CMD register
#define PORT_CMD_ST   (1u << 0)   // Start (DMA engine)
#define PORT_CMD_FRE  (1u << 4)   // FIS Receive Enable
#define PORT_CMD_FR   (1u << 14)  // FIS Receive Running
#define PORT_CMD_CR   (1u << 15)  // Command List Running

// Port SSTS register: device detection in bits 3:0
#define PORT_SSTS_DET_PRESENT  0x3

// Port TFD register: task file status bits
#define PORT_TFD_BSY  (1u << 7)
#define PORT_TFD_DRQ  (1u << 3)

// Port IS register: error flags
#define PORT_IS_TFES  (1u << 30)  // Task File Error Status

// Port IE (Interrupt Enable) bits
#define PORT_IE_DHRS  (1u << 0)   // Device to Host Register FIS (command done)
#define PORT_IE_TFES  (1u << 30)  // Task File Error

// Signatures
#define SATA_SIG_ATA   0x00000101u
#define SATA_SIG_ATAPI 0xEB140101u

// ATA commands
#define ATA_CMD_READ_DMA_EXT   0x25u
#define ATA_CMD_WRITE_DMA_EXT  0x35u

// FIS types
#define FIS_TYPE_H2D  0x27u

// ── On-disk structures (MMIO layout) ─────────────────────────────────────

typedef volatile struct {
    uint32_t clb;       // 0x00 Command List Base Address
    uint32_t clbu;      // 0x04 Command List Base Upper
    uint32_t fb;        // 0x08 FIS Base Address
    uint32_t fbu;       // 0x0C FIS Base Upper
    uint32_t is;        // 0x10 Interrupt Status
    uint32_t ie;        // 0x14 Interrupt Enable
    uint32_t cmd;       // 0x18 Command and Status
    uint32_t rsv0;
    uint32_t tfd;       // 0x20 Task File Data
    uint32_t sig;       // 0x24 Signature
    uint32_t ssts;      // 0x28 Serial ATA Status
    uint32_t sctl;      // 0x2C Serial ATA Control
    uint32_t serr;      // 0x30 Serial ATA Error
    uint32_t sact;      // 0x34 Serial ATA Active
    uint32_t ci;        // 0x38 Command Issue
    uint32_t sntf;      // 0x3C SATA Notification
    uint32_t fbs;       // 0x40 FIS-based Switching
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;           // 0x80 bytes per port

typedef volatile struct {
    uint32_t   cap;     // 0x00 Capabilities
    uint32_t   ghc;     // 0x04 Global Host Control
    uint32_t   is;      // 0x08 Interrupt Status
    uint32_t   pi;      // 0x0C Ports Implemented
    uint32_t   vs;      // 0x10 Version
    uint32_t   ccc_ctl; // 0x14
    uint32_t   ccc_pts; // 0x18
    uint32_t   em_loc;  // 0x1C
    uint32_t   em_ctl;  // 0x20
    uint32_t   cap2;    // 0x24
    uint32_t   bohc;    // 0x28
    uint8_t    rsv[0xA0 - 0x2C];
    uint8_t    vendor[0x100 - 0xA0];
    hba_port_t ports[32];  // starts at 0x100
} hba_mem_t;

// ── DMA buffer structures (in RAM, physical addresses given to AHCI) ───────

// Command header: 32 bytes, 32 per port → 1 KB command list
typedef struct {
    uint16_t flags;    // bits4:0=CFL, bit6=W(write), rest=0
    uint16_t prdtl;    // number of PRD entries
    uint32_t prdbc;    // PRD byte count (filled by hardware)
    uint32_t ctba;     // command table base address (low 32)
    uint32_t ctbau;    // command table base address (high 32)
    uint32_t rsv[4];
} __attribute__((packed)) cmd_header_t;  // 32 bytes

// Physical Region Descriptor entry
typedef struct {
    uint32_t dba;      // data buffer physical address (low 32)
    uint32_t dbau;     // data buffer physical address (high 32)
    uint32_t rsv;
    uint32_t dbc;      // byte count (bits 21:0), bit31 = interrupt on completion
} __attribute__((packed)) prdt_entry_t;  // 16 bytes

// Command table: FIS area + optional ATAPI area + PRDT
typedef struct {
    uint8_t     cfis[64];  // Command FIS (H2D)
    uint8_t     acmd[16];  // ATAPI command (unused)
    uint8_t     rsv[48];   // Reserved
    prdt_entry_t prdt[1];  // One PRD entry (up to 4MB per command)
} __attribute__((packed)) cmd_table_t;   // 144 bytes

// ── Driver state ──────────────────────────────────────────────────────────

static hba_mem_t*  s_hba  = NULL;  // virtual address of ABAR
static hba_port_t* s_port = NULL;  // the first active port

// IRQ line (0–15) assigned to the AHCI controller, 0xFF = unknown/disabled.
uint8_t g_ahci_irq = 0xFF;   // read by ahci_irq_entry in irq_stubs.asm

extern void ahci_irq_entry(void);

// Physical addresses of DMA buffers (given to AHCI in CLB/FB/CTBA).
// Virtual addresses = phys + HHDM_OFFSET (all PMM allocations are in RAM ⊂ HHDM).
static phys_addr_t s_cmdlist_phys;   // 4 KB, holds 32 × cmd_header_t
static phys_addr_t s_fis_phys;       // 4 KB, FIS receive buffer
static phys_addr_t s_cmdtbl_phys;    // 4 KB, cmd_table_t for slot 0

// DMA bounce buffer: callers may pass static kernel pointers (not HHDM-based),
// so we always DMA into/out of this PMM-allocated buffer and memcpy manually.
// order=3 → 8 pages = 32 KB = 64 sectors per chunk.
#define DMA_ORDER   3
#define DMA_SECTORS (8u * PAGE_SIZE / 512u)   // 64 sectors = 32 KB
static phys_addr_t s_dma_phys;

// ── I/O request queue ────────────────────────────────────────────────────
// After the scheduler starts, all disk I/O goes through a dedicated kthread.
// Callers submit requests and sleep; the kthread processes them serially
// (it's the sole owner of the AHCI hardware) and wakes each caller on
// completion.  Before the kthread starts, do_rw uses direct polling.
//
// Phase 9-6f: each request is a stack-allocated struct on the submitter's
// own kstack.  Submitters CAS-prepend onto s_req_head (lock-free MPSC
// Treiber stack); the kthread atomically xchg-drains the list, reverses
// it to restore FIFO order, and processes each entry.  No fixed ring,
// no producer ordering bug, no ring-full backpressure, no new locks.

#define AHCI_MAX_PAGES 16  // max pages per scatter-gather request (64KB)

typedef struct ahci_req {
    struct ahci_req* next;  // MPSC link — writers CAS onto s_req_head
    uint64_t lba;
    void*    buf;           // kernel-space buffer (used if page_count == 0)
    uint32_t count;         // sector count
    uint8_t  write;
    task_t*  waiter;
    volatile uint8_t done;
    uint8_t  result;
    // Scatter-gather: pre-resolved HHDM page pointers for user buffers.
    // If page_count > 0, the kthread scatters/gathers DMA data across
    // these pages instead of using `buf`.  Each entry points to the
    // start of a 4KB page (via HHDM).
    uint8_t* pages[AHCI_MAX_PAGES];
    uint32_t page_count;         // 0 = use buf; >0 = scatter-gather
    uint32_t first_page_offset;  // byte offset within pages[0]
} ahci_req_t;

static task_t* s_io_thread = NULL;     // the kthread task_t

// Phase 9-6f: AHCI submit is an unbounded lock-free MPSC of
// stack-allocated ahci_req_t pointers.  One CAS per producer, one
// xchg per consumer drain, zero new locks.
//
// Compared to the pre-9-6f fixed-ring design, this trades:
//   - static ring storage (AHCI_MAX_REQS * sizeof(ahci_req_t))
//     for stack allocation in the submitter's own frame (the
//     request is valid until ahci_submit returns because the
//     submitter is sleeping in its own frame on r->done).
//   - multi-producer index race on s_req_tail (which was a real
//     SMP bug under round-robin placement — two producers could
//     claim the same slot) for lock-free CAS-prepend onto one
//     head pointer.
//   - busy-yield on ring-full (violated the "no busy-wait" rule)
//     for no-ring-full: the MPSC is bounded only by the number
//     of live tasks.
//
// Correctness chain:
//   1. Producer fills every field of its on-stack ahci_req_t.
//   2. Producer CAS-prepends onto s_req_head (ATOMIC_RELEASE on
//      success).  Fields are ordered before the release.
//   3. Producer calls wait_queue_wake_all(&s_io_thread_wq), which
//      is an ACQ_REL xchg on the kthread's wait queue — ordered
//      after the CAS.
//   4. kthread drains s_io_thread_wq (finds its own task_we_t),
//      wakes, re-enters its loop, __atomic_exchange_n's s_req_head
//      to NULL grabbing the whole producer chain, reverses the
//      chain to restore FIFO, and walks it.  The xchg's acquire
//      pairs with every producer's CAS-release.
//   5. kthread processes the request, stores r->done=1 with
//      __ATOMIC_RELEASE, sched_wake(r->waiter).
//   6. Producer's sched_sleep loop reads r->done with
//      __ATOMIC_ACQUIRE; pairs with step 5.
static ahci_req_t* volatile s_req_head = NULL;
static wait_queue_t         s_io_thread_wq;

// ── Helpers ───────────────────────────────────────────────────────────────

static void udelay(uint32_t us) {
    // Burn ~1 µs per iteration at ~1 GHz (TCG is slow, add slack).
    for (volatile uint32_t i = 0; i < us * 200; i++);
}

static void port_stop(hba_port_t* p) {
    p->cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    // Wait for CR and FR to clear (up to 500 ms).
    for (int i = 0; i < 500; i++) {
        if (!(p->cmd & (PORT_CMD_FR | PORT_CMD_CR))) break;
        udelay(1000);
    }
}

static void port_start(hba_port_t* p) {
    // Wait for CR to clear first.
    while (p->cmd & PORT_CMD_CR) udelay(100);
    p->cmd |= PORT_CMD_FRE | PORT_CMD_ST;
}

// ── Port initialization ───────────────────────────────────────────────────

static void port_init(hba_port_t* p) {
    port_stop(p);

    // Allocate DMA buffers from the PMM (returns 4KB-aligned physical addresses).
    s_cmdlist_phys = pmm_buddy_alloc(0);          // 4 KB  — command list
    s_fis_phys     = pmm_buddy_alloc(0);          // 4 KB  — FIS receive buffer
    s_cmdtbl_phys  = pmm_buddy_alloc(0);          // 4 KB  — command table
    s_dma_phys     = pmm_buddy_alloc(DMA_ORDER);  // 32 KB — bounce buffer

    // Zero all three buffers via HHDM.
    uint8_t* cl = (uint8_t*)(s_cmdlist_phys + HHDM_OFFSET);
    uint8_t* fb = (uint8_t*)(s_fis_phys     + HHDM_OFFSET);
    uint8_t* ct = (uint8_t*)(s_cmdtbl_phys  + HHDM_OFFSET);
    __builtin_memset(cl, 0, 4096);
    __builtin_memset(fb, 0, 4096);
    __builtin_memset(ct, 0, 4096);
    uint8_t* dma = (uint8_t*)(s_dma_phys + HHDM_OFFSET);
    __builtin_memset(dma, 0, (1u << DMA_ORDER) * PAGE_SIZE);

    // Point command header 0 at the command table.
    cmd_header_t* hdr = (cmd_header_t*)cl;
    hdr[0].ctba  = (uint32_t)(s_cmdtbl_phys & 0xFFFFFFFF);
    hdr[0].ctbau = (uint32_t)(s_cmdtbl_phys >> 32);

    // Program port registers.
    p->clb  = (uint32_t)(s_cmdlist_phys & 0xFFFFFFFF);
    p->clbu = (uint32_t)(s_cmdlist_phys >> 32);
    p->fb   = (uint32_t)(s_fis_phys & 0xFFFFFFFF);
    p->fbu  = (uint32_t)(s_fis_phys >> 32);

    // Clear pending interrupts and errors.
    p->is   = p->is;    // write-to-clear
    p->serr = p->serr;

    // Enable interrupts for this port: command completion + errors.
    p->ie = PORT_IE_DHRS | PORT_IE_TFES;

    port_start(p);
}

// ── AHCI IRQ handler (called from ahci_irq_entry after early EOI) ─────────
void ahci_irq_handler(void) {
    if (!s_hba || !s_port) return;

    // Read which ports fired (write-to-clear after handling).
    uint32_t hba_is = s_hba->is;
    if (!hba_is) return;

    // Clear port IS (each bit is W1C).
    uint32_t port_is = s_port->is;
    s_port->is = port_is;

    // Clear the HBA global IS for the port we handled.
    s_hba->is = hba_is;

    irq_notify(g_ahci_irq);
}

// ── Issue one chunk command via bounce buffer ─────────────────────────────
// count must be <= DMA_SECTORS.
// `use_irq`: 0 = busy-poll (early boot), 1 = IRQ-wait (kthread).

static uint8_t issue_one(uint64_t lba, uint32_t count, uint8_t write,
                          uint8_t use_irq) {
    hba_port_t* p = s_port;

    // Wait for port idle: BSY=0 and DRQ=0.
    for (int i = 0; i < 100000; i++) {
        if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) break;
        if (i == 99999) return 0;
        udelay(10);
    }

    uint32_t bytes = count * 512;

    // Command header (slot 0).
    cmd_header_t* hdr = (cmd_header_t*)(s_cmdlist_phys + HHDM_OFFSET);
    hdr[0].flags = 5 | (write ? (1u << 6) : 0);
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    // PRD entry → points at bounce buffer.
    cmd_table_t* ct = (cmd_table_t*)(s_cmdtbl_phys + HHDM_OFFSET);
    ct->prdt[0].dba  = (uint32_t)(s_dma_phys & 0xFFFFFFFF);
    ct->prdt[0].dbau = (uint32_t)(s_dma_phys >> 32);
    ct->prdt[0].rsv  = 0;
    ct->prdt[0].dbc  = (bytes - 1) & 0x3FFFFF;

    // H2D FIS.
    uint8_t* fis = ct->cfis;
    __builtin_memset(fis, 0, 64);
    fis[0]  = FIS_TYPE_H2D;
    fis[1]  = 0x80;
    fis[2]  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis[4]  = (uint8_t)(lba        & 0xFF);
    fis[5]  = (uint8_t)((lba >>  8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[12] = (uint8_t)(count       & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);

    p->is   = p->is;
    p->serr = p->serr;

    p->ci   = 1u;

    if (use_irq) {
        // Hybrid: spin briefly for fast DMA, fall back to IRQ sleep.
        for (int spin = 0; spin < 4000; spin++) {
            if (!(p->ci & 1u)) goto done;
            if (p->is & PORT_IS_TFES) return 0;
        }
        irq_drain(g_ahci_irq);
        do {
            if (p->is & PORT_IS_TFES) return 0;
            irq_wait(g_ahci_irq);
        } while (p->ci & 1u);
    } else {
        // Polling: used during early boot (no scheduler).
        for (int i = 0; i < 3000000; i++) {
            if (!(p->ci & 1u)) break;
            if (p->is & PORT_IS_TFES) return 0;
            if (i == 2999999) return 0;
            udelay(10);
        }
    }
done:
    return (p->is & PORT_IS_TFES) ? 0 : 1;
}

// ── Direct I/O (polling) — used before kthread starts ─────────────────────

static uint8_t do_rw_direct(uint64_t lba, void* buf, uint32_t count,
                             uint8_t write) {
    if (!s_port) return 0;
    uint8_t* p8 = (uint8_t*)buf;

    while (count > 0) {
        uint32_t n     = count < DMA_SECTORS ? count : DMA_SECTORS;
        uint32_t bytes = n * 512;
        uint8_t* dma   = (uint8_t*)(s_dma_phys + HHDM_OFFSET);

        if (write) __builtin_memcpy(dma, p8, bytes);

        if (!issue_one(lba, n, write, 0)) return 0;

        if (!write) __builtin_memcpy(p8, dma, bytes);

        p8    += bytes;
        lba   += n;
        count -= n;
    }
    return 1;
}

// ── I/O kthread ──────────────────────────────────────────────────────────
// Sole owner of the AHCI hardware after boot.  Processes requests serially,
// uses IRQ-based wait, wakes the submitter when done.

// Copy between a DMA bounce buffer and a scatter-gather page list.
// `to_dma`=1: pages→dma (write path). `to_dma`=0: dma→pages (read path).
static void scatter_copy(uint8_t* dma, uint32_t bytes,
                         uint8_t** pages, uint32_t page_count,
                         uint32_t first_offset, uint32_t buf_offset,
                         uint8_t to_dma) {
    uint32_t pos = buf_offset;  // position in the logical user buffer
    uint32_t done = 0;
    while (done < bytes) {
        // Which page does `pos` land in?
        uint32_t abs = first_offset + pos;
        uint32_t pg  = abs / PAGE_SIZE;
        uint32_t off = abs % PAGE_SIZE;
        if (pg >= page_count) break;
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > bytes - done) chunk = bytes - done;
        uint8_t* p = pages[pg] + off;
        if (to_dma) __builtin_memcpy(dma + done, p, chunk);
        else        __builtin_memcpy(p, dma + done, chunk);
        done += chunk;
        pos  += chunk;
    }
}

// Process one request end-to-end: copy in (for writes), issue the DMA
// command(s), copy out (for reads), publish r->done and wake the waiter.
static void ahci_process_one(ahci_req_t* r) {
    uint64_t lba   = r->lba;
    uint32_t count = r->count;
    uint8_t  write = r->write;
    uint8_t  ok    = 1;

    uint8_t  scatter = (r->page_count > 0);
    uint8_t* p8      = (uint8_t*)r->buf;  // used only if !scatter
    uint32_t sg_off  = 0;                 // byte offset into scatter buffer

    while (count > 0 && ok) {
        uint32_t n     = count < DMA_SECTORS ? count : DMA_SECTORS;
        uint32_t bytes = n * 512;
        uint8_t* dma   = (uint8_t*)(s_dma_phys + HHDM_OFFSET);

        if (write) {
            if (scatter)
                scatter_copy(dma, bytes, r->pages, r->page_count,
                             r->first_page_offset, sg_off, 1);
            else
                __builtin_memcpy(dma, p8, bytes);
        }

        ok = issue_one(lba, n, write, 1);

        if (ok && !write) {
            if (scatter)
                scatter_copy(dma, bytes, r->pages, r->page_count,
                             r->first_page_offset, sg_off, 0);
            else
                __builtin_memcpy(p8, dma, bytes);
        }

        if (!scatter) p8 += bytes;
        sg_off += bytes;
        lba    += n;
        count  -= n;
    }

    r->result = ok;
    // RELEASE store pairs with the submitter's ACQUIRE load of r->done
    // in ahci_submit's sleep loop.  All the r->buf / scatter-gather
    // memcpy's above are ordered before this store.
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    if (r->waiter) sched_wake(r->waiter);
}

static void ahci_io_thread(void) {
    task_we_t self;

    for (;;) {
        // Canonical Phase 9-6 wait pattern on the kthread's own
        // queue, with the MPSC drain as the condition.
        //
        // Phase 1: peek.
        ahci_req_t* chain = __atomic_exchange_n(&s_req_head, (ahci_req_t*)NULL,
                                                  __ATOMIC_ACQUIRE);
        if (!chain) {
            // Phase 2: register on the kthread wait queue.
            task_we_init(&self, g_current);
            task_we_add(&s_io_thread_wq, &self);

            // Phase 3: re-check — a producer may have CAS-pushed
            // between phase 1 and phase 2.
            chain = __atomic_exchange_n(&s_req_head, (ahci_req_t*)NULL,
                                          __ATOMIC_ACQUIRE);
            if (!chain) sched_sleep();  // phase 4
            task_we_remove(&s_io_thread_wq, &self);

            // After waking we may still have an empty list (spurious
            // wake from a producer that saw the non-empty queue and
            // didn't bother waking): fall back through to another
            // xchg before processing.  But only drain once on the
            // post-wake path; the outer for(;;) re-enters and runs
            // phase 1 again with a fresh peek.
            if (!chain) {
                chain = __atomic_exchange_n(&s_req_head, (ahci_req_t*)NULL,
                                              __ATOMIC_ACQUIRE);
                if (!chain) continue;
            }
        }

        // Drain order is LIFO (CAS-prepend onto head); reverse in
        // place so we process requests in the order they were
        // submitted (FIFO) — matches the pre-9-6f fixed-ring behaviour.
        ahci_req_t* fifo = NULL;
        while (chain) {
            ahci_req_t* next = chain->next;
            chain->next = fifo;
            fifo = chain;
            chain = next;
        }

        while (fifo) {
            ahci_req_t* r = fifo;
            fifo = r->next;
            r->next = NULL;
            ahci_process_one(r);
        }
    }
}

// ── Resolve user-space buffer to HHDM kernel pointer ────────���────────────
// If `buf` is a user-space address (lower half), walk the current process's
// page tables to find the physical frame and return the HHDM pointer.
// If `buf` is already a kernel address (upper half / HHDM), return as-is.
// This lets the I/O kthread safely access the buffer from any context,
// since HHDM is always mapped regardless of which process's CR3 is loaded.
// NOTE: only works for buffers that fit within a single page.  For multi-page
// buffers, the caller must ensure each page is resolved (or use a kernel buf).
static void* resolve_to_hhdm(void* buf) {
    uint64_t va = (uint64_t)buf;
    if (va >= HHDM_OFFSET) return buf;  // already kernel space
    phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
    uint64_t page_va = va & ~0xFFFULL;
    phys_addr_t phys = vmm_page_phys(pml4, page_va);
    if (phys == PMM_INVALID_ADDR) return buf;  // can't resolve — hope for the best
    return (void*)(phys + HHDM_OFFSET + (va & 0xFFFULL));
}

// ── Submit a request to the I/O thread and sleep until completion ────────

// CAS-prepend an ahci_req_t onto s_req_head.  The request's `next`
// pointer is written inside the CAS loop so a producer retry is safe.
// Pairs (release) with the kthread's xchg-acquire drain.
static inline void ahci_req_push(ahci_req_t* r) {
    ahci_req_t* old_head = __atomic_load_n(&s_req_head, __ATOMIC_RELAXED);
    do {
        r->next = old_head;
    } while (!__atomic_compare_exchange_n(&s_req_head, &old_head, r,
                                            /*weak=*/0,
                                            __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED));
}

// Block until the kthread publishes r->done=1 (release).  Uses the
// canonical sleep pattern: the read is under ACQUIRE so it pairs
// with the kthread's RELEASE store.
static inline void ahci_wait_done(ahci_req_t* r) {
    for (;;) {
        if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) return;
        sched_sleep();
    }
}

static uint8_t ahci_submit(uint64_t lba, void* buf, uint32_t count,
                            uint8_t write) {
    // Resolve user-space buffer to HHDM pointer so the kthread can access it.
    // Multi-page buffers: we resolve page-by-page by checking if buf is in
    // user space.  For now, all callers pass kernel-space buffers (block cache,
    // static buffers) so this is a safety net — the fast path is a no-op.
    void* kbuf = resolve_to_hhdm(buf);

    // Stack-allocate the request in the caller's own frame.  It lives
    // until ahci_wait_done returns below; the kthread's reference is
    // bounded by that interval.
    ahci_req_t r;
    r.next              = NULL;
    r.lba               = lba;
    r.buf               = kbuf;
    r.count             = count;
    r.write             = write;
    r.waiter            = g_current;
    r.done              = 0;
    r.result            = 0;
    r.page_count        = 0;
    r.first_page_offset = 0;

    ahci_req_push(&r);

    // Wake the I/O thread if it's parked.  The wake path is the
    // kthread's own wait queue, not a direct sched_wake by name, so
    // any future kthread identity (init-time restart) is handled
    // transparently.
    wait_queue_wake_all(&s_io_thread_wq);

    ahci_wait_done(&r);
    return r.result;
}

// ── Scatter-gather submit: resolve user pages and DMA directly ───────────
// Called from ext2's multi-block path.  Resolves the user buffer pages
// via vmm_get_user_pages() so the kthread can write DMA data directly
// to the user's physical frames via HHDM — zero extra copies.

static uint8_t ahci_submit_sg(uint64_t lba, void* user_buf,
                               uint32_t count, uint8_t write) {
    uint64_t va = (uint64_t)user_buf;
    uint32_t total_bytes = count * 512;
    uint32_t first_off = (uint32_t)(va & 0xFFF);
    uint32_t npages = (first_off + total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (npages > AHCI_MAX_PAGES) return 0;  // too large for scatter list

    phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
    void* page_ptrs[AHCI_MAX_PAGES];
    if (!vmm_get_user_pages(pml4, va, npages, page_ptrs))
        return 0;

    // Pin every resolved frame so CoW fork won't share them while DMA
    // is in flight.  The kthread writes via HHDM — if fork CoW-shared
    // the frame, the child would see DMA data it shouldn't.
    phys_addr_t pin_addrs[AHCI_MAX_PAGES];
    for (uint32_t i = 0; i < npages; i++) {
        pin_addrs[i] = (phys_addr_t)((uint64_t)page_ptrs[i] - HHDM_OFFSET);
        pmm_pin(pin_addrs[i]);
    }

    ahci_req_t r;
    r.next              = NULL;
    r.lba               = lba;
    r.buf               = NULL;
    r.count             = count;
    r.write             = write;
    r.waiter            = g_current;
    r.done              = 0;
    r.result            = 0;
    r.page_count        = npages;
    r.first_page_offset = first_off;
    for (uint32_t i = 0; i < npages; i++)
        r.pages[i] = (uint8_t*)page_ptrs[i];

    ahci_req_push(&r);
    wait_queue_wake_all(&s_io_thread_wq);
    ahci_wait_done(&r);

    // Unpin after DMA is complete.
    for (uint32_t i = 0; i < npages; i++)
        pmm_unpin(pin_addrs[i]);

    return r.result;
}

// ── Public API ────────────────────────────────────────────────────────────

uint8_t ahci_read(uint64_t lba, void* buf, uint32_t count) {
    if (!s_port) return 0;
    if (s_io_thread) return ahci_submit(lba, buf, count, 0);
    return do_rw_direct(lba, buf, count, 0);
}

uint8_t ahci_write(uint64_t lba, const void* buf, uint32_t count) {
    if (!s_port) return 0;
    if (s_io_thread) return ahci_submit(lba, (void*)buf, count, 1);
    return do_rw_direct(lba, (void*)buf, count, 1);
}

// Read directly into a user-space buffer via scatter-gather.
// Resolves user pages to HHDM, DMA bounce → pages.  Zero extra copies.
uint8_t ahci_read_user(uint64_t lba, void* user_buf, uint32_t count) {
    if (!s_port) return 0;
    if (s_io_thread) return ahci_submit_sg(lba, user_buf, count, 0);
    return do_rw_direct(lba, user_buf, count, 0);
}

// ── Start the I/O kthread (call after sched_init) ────────────────────────

void ahci_start_io_thread(void) {
    if (g_ahci_irq == 0xFF) return;  // no MSI — stay in polling mode
    wait_queue_init(&s_io_thread_wq);
    s_io_thread = task_create_kthread(ahci_io_thread, pid_alloc());
    if (s_io_thread) sched_add(s_io_thread);
}

// ── ahci_init ─────────────────────────────────────────────────────────────

uint8_t ahci_init(void) {
    // 1. Find AHCI controller on PCI bus.
    pci_device_t dev;
    if (!pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_AHCI, &dev)) return 0;

    // 2. Enable MMIO + bus mastering.
    pci_enable(dev.bus, dev.dev, dev.fn);

    // 2b. Disable MSI and MSI-X to force legacy INTx through the 8259A PIC.
    //     ich9-ahci may use MSI-X (cap 0x11) even if MSI (cap 0x05) is off.
    {
        uint32_t status = pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x04) >> 16;
        if (status & (1u << 4)) {
            uint8_t cap = pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x34) & 0xFC;
            while (cap) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
                uint8_t  id = dw & 0xFF;
                if (id == 0x05)        // MSI: Enable = bit 16
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap, dw & ~(1u << 16));
                else if (id == 0x11)   // MSI-X: Enable = bit 31
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap, dw & ~(1u << 31));
                cap = ((dw >> 8) & 0xFF) & 0xFC;
            }
        }
    }

    // 3. Map ABAR (BAR5) into kernel virtual space.
    uint64_t abar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn, AHCI_BAR);
    if (!abar_phys) return 0;

    s_hba = (hba_mem_t*)vmm_map_mmio(abar_phys, sizeof(hba_mem_t));

    // 4. Enable AHCI mode.
    s_hba->ghc |= HBA_GHC_AE;

    // 5. Find the first port that has a SATA drive.
    uint32_t pi = s_hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;

        hba_port_t* p = &s_hba->ports[i];

        // Check device presence: SSTS.DET == 3 (device + communication).
        if ((p->ssts & 0xF) != PORT_SSTS_DET_PRESENT) continue;

        // Accept SATA drives only (not ATAPI, PM, etc.).
        if (p->sig != SATA_SIG_ATA) continue;

        s_port = p;
        port_init(p);

        // 6. Enable global HBA interrupt delivery.
        s_hba->ghc |= HBA_GHC_IE;

        // 7. Enable MSI and register the ISR.
        //
        // MSI bypasses the IOAPIC and PIC entirely: the device writes a
        // LAPIC-format message directly to memory when it needs attention.
        // We locate the MSI capability, program the LAPIC address and vector,
        // then enable MSI.  No PIIX3/PIRQ table programming needed.
        {
            // Locate the MSI capability (ID = 0x05) in the PCI cap list.
            uint8_t cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                                   0x34u) & 0xFCu);
            int msi_found = 0;
            while (cap) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
                if ((dw & 0xFFu) == 0x05u) {  // MSI capability
                    // Message Control register (bits [31:16] of dword at cap).
                    // bit 16 = Enable, bits [19:17] = Multiple Message Enable.
                    // We request single message (MME = 000).
                    uint32_t mc = dw;

                    // Write MSI address (LAPIC address for BSP).
                    uint64_t msi_addr = lapic_msi_addr();
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                    cap + 4u, (uint32_t)(msi_addr & 0xFFFFFFFFu));

                    int is_64bit = (mc >> 23) & 1u;
                    if (is_64bit) {
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                        cap + 8u, (uint32_t)(msi_addr >> 32));
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                        cap + 12u, lapic_msi_data(VEC_AHCI_MSI));
                    } else {
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                        cap + 8u, lapic_msi_data(VEC_AHCI_MSI));
                    }

                    // Enable MSI (bit 16), single message (MME=000).
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap,
                                    (mc & ~(0x7u << 20)) | (1u << 16));

                    msi_found = 1;
                    break;
                }
                cap = (uint8_t)((dw >> 8) & 0xFCu);
            }

            // Register IDT entry and set the irq_wait key.
            // g_ahci_irq is repurposed as a logical slot index for irq_wait().
            // We reuse slot 11 (matches old PIIX3 IRQ) — irq_wait has 16 slots.
            g_ahci_irq = 11u;
            idt_irq_register(VEC_AHCI_MSI, (uint64_t)ahci_irq_entry);

            if (!msi_found) {
                // Fallback: should not happen on QEMU AHCI, but handle it.
                // Without MSI the driver will never get IRQs — AHCI will poll.
                g_ahci_irq = 0xFF;
            }
        }

        return 1;
    }

    return 0;
}
