#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "common.h"
#include "idt.h"
#include "lapic.h"
#include "sched.h"
#include "process.h"
#include "wait.h"
#include "smp.h"
#include "preempt.h"
#include "cpu.h"

// ── PCI class codes ────────────────────────────────────────────────────────
#define PCI_CLASS_STORAGE  0x01
#define PCI_SUBCLASS_AHCI  0x06
#define AHCI_BAR           5       // ABAR = BAR5

// ── HBA register bits ─────────────────────────────────────────────────────
#define HBA_GHC_AE   (1u << 31)   // AHCI Enable
#define HBA_GHC_IE   (1u << 1)    // Interrupt Enable (global)
#define HBA_GHC_HR   (1u << 0)    // HBA Reset

// HBA CAP bits
#define HBA_CAP_SNCQ (1u << 30)   // Supports Native Command Queuing

// Port CMD
#define PORT_CMD_ST   (1u << 0)   // Start
#define PORT_CMD_FRE  (1u << 4)   // FIS Receive Enable
#define PORT_CMD_FR   (1u << 14)  // FIS Receive Running
#define PORT_CMD_CR   (1u << 15)  // Command List Running

// Port SSTS
#define PORT_SSTS_DET_PRESENT  0x3

// Port TFD
#define PORT_TFD_BSY  (1u << 7)
#define PORT_TFD_DRQ  (1u << 3)

// Port IS / IE bits
#define PORT_IS_DHRS  (1u << 0)   // Device to Host Register FIS received
#define PORT_IS_SDBS  (1u << 3)   // Set Device Bits FIS received (NCQ done)
#define PORT_IS_TFES  (1u << 30)  // Task File Error Status

#define PORT_IE_DHRS  (1u << 0)
#define PORT_IE_SDBS  (1u << 3)
#define PORT_IE_TFES  (1u << 30)

// Signatures
#define SATA_SIG_ATA   0x00000101u
#define SATA_SIG_ATAPI 0xEB140101u

// ATA commands
#define ATA_CMD_READ_DMA_EXT         0x25u
#define ATA_CMD_WRITE_DMA_EXT        0x35u
#define ATA_CMD_READ_FPDMA_QUEUED    0x60u   // NCQ read
#define ATA_CMD_WRITE_FPDMA_QUEUED   0x61u   // NCQ write

// FIS type
#define FIS_TYPE_H2D  0x27u

// ── HBA MMIO register structures ─────────────────────────────────────────

typedef volatile struct {
    uint32_t clb;       // 0x00 Command List Base
    uint32_t clbu;      // 0x04 Command List Base Upper
    uint32_t fb;        // 0x08 FIS Base Address
    uint32_t fbu;       // 0x0C FIS Base Upper
    uint32_t is;        // 0x10 Interrupt Status
    uint32_t ie;        // 0x14 Interrupt Enable
    uint32_t cmd;       // 0x18 Command and Status
    uint32_t rsv0;
    uint32_t tfd;       // 0x20 Task File Data
    uint32_t sig;       // 0x24 Signature
    uint32_t ssts;      // 0x28 SATA Status
    uint32_t sctl;      // 0x2C SATA Control
    uint32_t serr;      // 0x30 SATA Error
    uint32_t sact;      // 0x34 SATA Active (NCQ in-flight bitmask)
    uint32_t ci;        // 0x38 Command Issue
    uint32_t sntf;      // 0x3C SATA Notification
    uint32_t fbs;       // 0x40 FIS-based Switching
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t   cap;
    uint32_t   ghc;
    uint32_t   is;
    uint32_t   pi;
    uint32_t   vs;
    uint32_t   ccc_ctl;
    uint32_t   ccc_pts;
    uint32_t   em_loc;
    uint32_t   em_ctl;
    uint32_t   cap2;
    uint32_t   bohc;
    uint8_t    rsv[0xA0 - 0x2C];
    uint8_t    vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

// ── DMA structures ─────────────────────────────────────────────────────────

typedef struct {
    uint16_t flags;    // [4:0]=CFL, [6]=W, [7]=P(prefetch)
    uint16_t prdtl;    // PRDT length (number of entries)
    uint32_t prdbc;    // PRDT byte count (filled by HBA)
    uint32_t ctba;     // command table base (low)
    uint32_t ctbau;    // command table base (high)
    uint32_t rsv[4];
} __attribute__((packed)) cmd_header_t;  // 32 bytes

typedef struct {
    uint32_t dba;      // data buffer physical address (low)
    uint32_t dbau;     // data buffer physical address (high)
    uint32_t rsv;
    uint32_t dbc;      // byte count-1 (bits[21:0]), bit[31]=interrupt on completion
} __attribute__((packed)) prdt_entry_t;  // 16 bytes

// Command table: one full 4KB page per slot.
// Header (128 bytes) + 248 PRDT entries × 16 bytes = 128 + 3968 = 4096 bytes.
// 248 entries × 4 KiB/entry = ~992 KiB max per NCQ command — more than enough
// for any realistic single I/O (AHCI_DMA_SECTORS = 1024 sectors = 512 KiB).
typedef struct {
    uint8_t      cfis[64];    // Command FIS (H2D)
    uint8_t      acmd[16];    // ATAPI command (unused)
    uint8_t      rsv[48];     // Reserved
    prdt_entry_t prdt[248];   // Physical Region Descriptor Table
} __attribute__((packed)) cmd_table_t;   // exactly 4096 bytes

// ── Driver state ──────────────────────────────────────────────────────────

static hba_mem_t*  s_hba  = NULL;
static hba_port_t* s_port = NULL;

// NCQ support: set at init from HBA CAP.SNCQ + device IDENTIFY.
// If 0, use READ/WRITE_DMA_EXT with a single slot.
static uint8_t  s_ncq    = 0;
static uint32_t s_nslots = 1;    // 1–32; capped by HBA CAP.NCS

// MSI-X table (16 bytes per entry, mapped via vmm_map_mmio).
static volatile uint32_t* s_msix_entry = NULL;  // points at entry 0

// ── Per-slot command tables ───────────────────────────────────────────────
// One 4KB page per slot, holds cmd_table_t.  Each slot always points to its
// own table so concurrent commands never share memory.
#define MAX_NCQ_SLOTS  32
static phys_addr_t s_ctbl_phys[MAX_NCQ_SLOTS];

// Shared command list (4KB = 32 × 128-byte cmd_header_t, one per slot).
static phys_addr_t s_cmdlist_phys;

// FIS receive buffer (4KB).
static phys_addr_t s_fis_phys;

// ── va_to_phys ────────────────────────────────────────────────────────────
// Convert any kernel virtual address to its physical address.
// Two kernel address ranges:
//   [HHDM_OFFSET, 0xFFFFFFFF80000000)  → HHDM direct mapping
//   [0xFFFFFFFF80000000, ...)           → kernel text / data / BSS
static inline phys_addr_t va_to_phys(const void* va) {
    uint64_t v = (uint64_t)va;
    if (v >= 0xFFFFFFFF80000000ULL)
        return v - 0xFFFFFFFF80000000ULL + KERNEL_BASE_PHYS;
    return v - HHDM_OFFSET;
}

// ── Slot management ───────────────────────────────────────────────────────
//
// s_free_mask:   bit N=1 → slot N is free (may be CAS-allocated)
// s_active_mask: bit N=1 → slot N has an in-flight NCQ command
//                (or a non-NCQ command if s_ncq==0)
//
// Slot allocation: CAS on s_free_mask in slot_alloc().
// Slot release:    OR back into s_free_mask by the IRQ handler when done.
//
// SACT / CI write race:
//   Multiple CPUs can be in issue_ncq() simultaneously (different slots).
//   Both SACT and CI are memory-mapped 32-bit registers; a read-modify-write
//   (|=) from two CPUs is NOT atomic.  s_ci_lock serialises the SACT|=bit
//   and CI|=bit pair so each slot's "SACT before CI" ordering is guaranteed
//   and no write is lost.  The critical section is ≤ 4 MMIO writes, so the
//   spinlock is held for nanoseconds — no starvation risk.
static volatile uint32_t s_free_mask;
static volatile uint32_t s_active_mask;
static spinlock_t         s_ci_lock;

// s_slot_avail_wq: tasks sleeping here are waiting for a free slot.
// Woken by the IRQ handler whenever it releases one or more slots.
static wait_queue_t   s_slot_avail_wq;

// Per-slot wait queue, done flag, and result.
// IRQ handler sets s_slot_done[slot]=1 (RELEASE) and wakes s_slot_wq[slot].
// Submitter checks s_slot_done[slot] (ACQUIRE) in slot_wait().
static wait_queue_t   s_slot_wq[MAX_NCQ_SLOTS];
static volatile uint8_t s_slot_done[MAX_NCQ_SLOTS];
static volatile uint8_t s_slot_result[MAX_NCQ_SLOTS];

// Set to 1 by ahci_start_io_thread() after sched_init() completes.
// Gates the IRQ/NCQ path; pre-scheduler code uses do_rw_direct().
static uint8_t s_irq_ready = 0;

// IRQ line / logical slot (kept for compatibility with irq_stubs.asm).
uint8_t g_ahci_irq = 0xFF;
extern void ahci_irq_entry(void);

// ── Helpers ───────────────────────────────────────────────────────────────

static void udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 200; i++);
}

static void port_stop(hba_port_t* p) {
    p->cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    for (int i = 0; i < 500; i++) {
        if (!(p->cmd & (PORT_CMD_FR | PORT_CMD_CR))) break;
        udelay(1000);
    }
}

static void port_start(hba_port_t* p) {
    while (p->cmd & PORT_CMD_CR) udelay(100);
    p->cmd |= PORT_CMD_FRE | PORT_CMD_ST;
}

// ── Port initialisation ───────────────────────────────────────────────────

static void port_init(hba_port_t* p) {
    port_stop(p);

    // Shared structures
    s_cmdlist_phys = pmm_buddy_alloc(0);
    s_fis_phys     = pmm_buddy_alloc(0);
    __builtin_memset((void*)(s_cmdlist_phys + HHDM_OFFSET), 0, PAGE_SIZE);
    __builtin_memset((void*)(s_fis_phys     + HHDM_OFFSET), 0, PAGE_SIZE);

    // Per-slot command tables (one 4KB page each).
    // s_nslots is set before port_init is called.
    cmd_header_t* hdr = (cmd_header_t*)(s_cmdlist_phys + HHDM_OFFSET);
    for (uint32_t i = 0; i < s_nslots; i++) {
        s_ctbl_phys[i] = pmm_buddy_alloc(0);
        __builtin_memset((void*)(s_ctbl_phys[i] + HHDM_OFFSET), 0, PAGE_SIZE);
        hdr[i].ctba  = (uint32_t)(s_ctbl_phys[i] & 0xFFFFFFFF);
        hdr[i].ctbau = (uint32_t)(s_ctbl_phys[i] >> 32);
    }

    // Slot management
    s_free_mask   = (s_nslots == 32) ? 0xFFFFFFFFu : ((1u << s_nslots) - 1u);
    s_active_mask = 0;
    spin_lock_init(&s_ci_lock);
    wait_queue_init(&s_slot_avail_wq);
    for (uint32_t i = 0; i < s_nslots; i++) {
        wait_queue_init(&s_slot_wq[i]);
        s_slot_done[i]   = 0;
        s_slot_result[i] = 0;
    }

    // Program port registers
    p->clb  = (uint32_t)(s_cmdlist_phys & 0xFFFFFFFF);
    p->clbu = (uint32_t)(s_cmdlist_phys >> 32);
    p->fb   = (uint32_t)(s_fis_phys & 0xFFFFFFFF);
    p->fbu  = (uint32_t)(s_fis_phys >> 32);
    p->is   = p->is;    // W1C — clear pending
    p->serr = p->serr;

    // Enable port interrupts.
    // DHRS: D2H FIS (non-NCQ command done).
    // SDBS: Set Device Bits FIS (NCQ command done).
    // TFES: task file error.
    p->ie = PORT_IE_DHRS | PORT_IE_SDBS | PORT_IE_TFES;

    port_start(p);
}

// ── IRQ handler ───────────────────────────────────────────────────────────
// Called from ahci_irq_entry (asm stub) after lapic_eoi().
// Runs on whichever CPU the MSI-X lowest-priority delivery chose.
// Wakes the task that submitted the completed slot — usually on a different CPU.

void ahci_irq_handler(void) {
    if (!s_hba || !s_port) return;

    uint32_t hba_is = s_hba->is;
    if (!hba_is) return;

    uint32_t port_is = s_port->is;
    s_port->is = port_is;   // W1C
    s_hba->is  = hba_is;    // W1C

    // Bump preempt_depth for the whole body.  The wait_queue_wake_all
    // calls below run rcu_read_lock/unlock; at depth 0 with
    // reschedule_pending set, preempt_enable would call sched_preempt →
    // do_switch → trailing `sti`, re-enabling IRQs inside the ISR and
    // letting a fresh AHCI MSI nest on top of us.  Staying at depth>=1
    // short-circuits that; any pending reschedule is picked up after
    // iretq.  Manual dec at exit (not preempt_enable, which would re-
    // open the same path).
    preempt_disable();

    if (port_is & PORT_IS_TFES) {
        // Task file error: fail all active slots.  The port is now dirty;
        // leaving it is safe for a restart (next submit re-issues).
        uint32_t active = __atomic_exchange_n(&s_active_mask, 0u, __ATOMIC_ACQ_REL);
        uint32_t mask = active;
        while (mask) {
            uint32_t slot = (uint32_t)__builtin_ctz(mask);
            mask &= mask - 1u;
            s_slot_result[slot] = 0;
            __atomic_store_n(&s_slot_done[slot], 1u, __ATOMIC_RELEASE);
            wait_queue_wake_all(&s_slot_wq[slot]);
        }
        __atomic_fetch_or(&s_free_mask, active, __ATOMIC_RELEASE);
        wait_queue_wake_all(&s_slot_avail_wq);
        this_cpu()->preempt_depth--;
        return;
    }

    // Detect completed slots.
    // NCQ:     device clears SACT bits (via Set Device Bits FIS).
    // Non-NCQ: HBA clears CI bits when D2H FIS is received.
    //
    // Loop until SACT/CI stabilises.  W1C of port_is clears ALL pending
    // interrupt bits atomically, including bits for completions that arrived
    // between our IS read and our IS W1C write.  Those completions have
    // their SACT bit cleared by the device but no new IRQ will fire (IS
    // was already W1C'd).  Re-scanning SACT in a loop drains them before
    // we return from the ISR.
    for (;;) {
        // Hold s_ci_lock across the active_mask+ci read.  issue_cmd
        // writes active_mask BEFORE ci under the same lock.  Without
        // this lock the ISR observes the window where active_mask bit
        // is set but ci bit still 0, computes done = bit & ~0 = bit,
        // and falsely completes a slot whose DMA hasn't been submitted
        // yet — subsequent slot reuse then corrupts the in-flight PRDT
        // and DMA lands in the wrong buffer.
        uint64_t flags = spin_lock_irqsave(&s_ci_lock);
        uint32_t active = __atomic_load_n(&s_active_mask, __ATOMIC_ACQUIRE);
        uint32_t done;
        if (s_ncq)
            done = active & ~s_port->sact;
        else
            done = active & ~s_port->ci;

        if (!done) { spin_unlock_irqrestore(&s_ci_lock, flags); break; }

        // Release from active_mask before waking waiters so a woken task
        // that re-submits immediately won't see its slot as still active.
        __atomic_fetch_and(&s_active_mask, ~done, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&s_ci_lock, flags);

        uint32_t mask = done;
        while (mask) {
            uint32_t slot = (uint32_t)__builtin_ctz(mask);
            mask &= mask - 1u;
            s_slot_result[slot] = 1;
            __atomic_store_n(&s_slot_done[slot], 1u, __ATOMIC_RELEASE);
            wait_queue_wake_all(&s_slot_wq[slot]);
        }

        // Free slots AFTER waking waiters so slot_avail_wq wake is last.
        __atomic_fetch_or(&s_free_mask, done, __ATOMIC_RELEASE);
        wait_queue_wake_all(&s_slot_avail_wq);
    }
    this_cpu()->preempt_depth--;
}

// ── Slot allocation ───────────────────────────────────────────────────────
// Returns a slot index 0..(s_nslots-1).  Sleeps if all slots are in use.
// Guaranteed to eventually return a free slot.

static uint32_t slot_alloc(void) {
    for (;;) {
        uint32_t free = __atomic_load_n(&s_free_mask, __ATOMIC_ACQUIRE);
        if (!free) {
            WAIT_EVENT(&s_slot_avail_wq,
                       __atomic_load_n(&s_free_mask, __ATOMIC_ACQUIRE) != 0);
            continue;
        }
        uint32_t slot = (uint32_t)__builtin_ctz(free);
        uint32_t bit  = 1u << slot;
        if (__atomic_compare_exchange_n(&s_free_mask, &free, free & ~bit,
                                         0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return slot;
        // CAS failed (another CPU grabbed concurrently): retry.
    }
}

// ── PRDT builder ─────────────────────────────────────────────────────────
// Build PRDT entries for a contiguous physical buffer.
// Returns the number of entries written (≤ 248).

static uint32_t build_prdt(prdt_entry_t* prdt, phys_addr_t phys, uint32_t bytes) {
    uint32_t n = 0;
    while (bytes > 0) {
        // Stay within the current 4 KiB page boundary.
        uint32_t off   = (uint32_t)(phys & (PAGE_SIZE - 1));
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > bytes) chunk = bytes;
        prdt[n].dba  = (uint32_t)(phys & 0xFFFFFFFFu);
        prdt[n].dbau = (uint32_t)(phys >> 32);
        prdt[n].rsv  = 0;
        prdt[n].dbc  = (chunk - 1u) & 0x3FFFFFu;
        n++;
        phys  += chunk;
        bytes -= chunk;
    }
    return n;
}

// Build PRDT from an array of distinct physical page addresses.
// pages[i] = 4 KiB-aligned physical frame; first_off = offset in pages[0].
// Returns number of entries written.

static uint32_t build_prdt_pages(prdt_entry_t* prdt,
                                  phys_addr_t* pages, uint32_t npages,
                                  uint32_t first_off, uint32_t bytes) {
    uint32_t n = 0, rem = bytes;
    for (uint32_t i = 0; i < npages && rem > 0; i++) {
        uint32_t off   = (i == 0) ? first_off : 0u;
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > rem) chunk = rem;
        phys_addr_t phys = pages[i] + off;
        prdt[n].dba  = (uint32_t)(phys & 0xFFFFFFFFu);
        prdt[n].dbau = (uint32_t)(phys >> 32);
        prdt[n].rsv  = 0;
        prdt[n].dbc  = (chunk - 1u) & 0x3FFFFFu;
        n++;
        rem -= chunk;
    }
    return n;
}

// ── Command issue ─────────────────────────────────────────────────────────
// Program the command header and FIS for `slot`, then pull the CI/SACT
// trigger.  The PRDT must already be written into s_ctbl_phys[slot].prdt[].
// nprdt: number of PRDT entries already written.

static void issue_cmd(uint32_t slot, uint64_t lba, uint32_t nsectors,
                      uint8_t write, uint16_t nprdt) {
    hba_port_t* p = s_port;

    // Command header
    cmd_header_t* hdr = (cmd_header_t*)(s_cmdlist_phys + HHDM_OFFSET);
    // CFL=5, W=write, P=prefetch for reads (helps HBA pipeline the PRDT fetch)
    hdr[slot].flags  = 5u | (write ? (1u << 6) : (1u << 7));
    hdr[slot].prdtl  = nprdt;
    hdr[slot].prdbc  = 0;
    // ctba/ctbau already set in port_init and never change.

    // H2D FIS
    uint8_t* fis = ((cmd_table_t*)(s_ctbl_phys[slot] + HHDM_OFFSET))->cfis;
    __builtin_memset(fis, 0, 64);
    fis[0] = FIS_TYPE_H2D;
    fis[1] = 0x80;  // C=1 (command register)

    if (s_ncq) {
        // NCQ FIS: sector count in Feature register; tag in Count register.
        fis[2]  = write ? ATA_CMD_WRITE_FPDMA_QUEUED : ATA_CMD_READ_FPDMA_QUEUED;
        fis[3]  = (uint8_t)(nsectors        & 0xFFu);  // feature lo = count lo
        fis[4]  = (uint8_t)(lba             & 0xFFu);
        fis[5]  = (uint8_t)((lba >>  8)     & 0xFFu);
        fis[6]  = (uint8_t)((lba >> 16)     & 0xFFu);
        fis[7]  = 0x40;                                // LBA48 mode
        fis[8]  = (uint8_t)((lba >> 24)     & 0xFFu);
        fis[9]  = (uint8_t)((lba >> 32)     & 0xFFu);
        fis[10] = (uint8_t)((lba >> 40)     & 0xFFu);
        fis[11] = (uint8_t)((nsectors >> 8) & 0xFFu);  // feature hi = count hi
        fis[12] = (uint8_t)(slot << 3);                 // count lo = NCQ tag
        fis[13] = 0;
    } else {
        // Non-NCQ DMA EXT: sector count in Count register.
        fis[2]  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        fis[4]  = (uint8_t)(lba             & 0xFFu);
        fis[5]  = (uint8_t)((lba >>  8)     & 0xFFu);
        fis[6]  = (uint8_t)((lba >> 16)     & 0xFFu);
        fis[7]  = 0x40;
        fis[8]  = (uint8_t)((lba >> 24)     & 0xFFu);
        fis[9]  = (uint8_t)((lba >> 32)     & 0xFFu);
        fis[10] = (uint8_t)((lba >> 40)     & 0xFFu);
        fis[12] = (uint8_t)(nsectors        & 0xFFu);
        fis[13] = (uint8_t)((nsectors >> 8) & 0xFFu);
    }

    uint32_t bit = 1u << slot;

    // s_ci_lock serialises SACT|=bit, CI|=bit, and active_mask|=bit as a
    // single unit.  active_mask MUST be set AFTER the command is visible to
    // the device (after CI write) so the IRQ handler never sees a slot as
    // "active but not yet issued" — which would cause it to compute a false
    // completion and wake the waiter before any data is transferred.
    // Correct ordering (inside the lock):
    //   1. sact  — tells device this tag is now active (NCQ only)
    //   2. active_mask — must be visible BEFORE ci so the IRQ handler can
    //      never miss a completion (device could complete the instant ci is
    //      written; if active_mask isn't set yet, the IRQ computes done=0 and
    //      slot_wait sleeps forever until the next unrelated IRQ rescans).
    //   3. ci — actually issues the command to the device
    // Holding s_ci_lock prevents a concurrent CPU's sact/ci RMW from racing.
    // irqsave: s_ci_lock is also taken in ahci_rescan_completions, which
    // runs in IRQ context via sched_tick → ahci_poll_completions.  If the
    // timer IRQ fires on this CPU while we hold the lock here in process
    // context, its rescan would self-deadlock.
    uint64_t ci_flags = spin_lock_irqsave(&s_ci_lock);
    p->is = p->is;
    if (s_ncq) p->sact |= bit;   // NCQ: set SACT before CI (AHCI spec)
    __atomic_fetch_or(&s_active_mask, bit, __ATOMIC_RELEASE);  // AFTER sact, BEFORE ci
    p->ci |= bit;
    spin_unlock_irqrestore(&s_ci_lock, ci_flags);
}

// ── Slot wait ─────────────────────────────────────────────────────────────
// Sleep until the IRQ handler marks s_slot_done[slot] = 1.

// Rescan SACT/CI for completions that the ISR may have missed due to
// interrupt coalescing: W1C of port_is can clear a completion's IS bit
// before the ISR reads SACT, leaving the slot done in hardware but with
// no further IRQ coming.  Called from slot_wait after each wakeup.
//
// MUST hold s_ci_lock across the active_mask+ci read.  issue_cmd sets
// active_mask BEFORE ci (so the ISR — which only fires AFTER ci is set —
// never races with it).  But this rescan can run from ANY wake (including
// a cross-slot wake from slot_avail_wq), i.e. during the issue_cmd window
// where active_mask is already set but ci is not.  Without the lock, rescan
// computes done = active & ~ci = bit & ~0 = bit → false completion on a slot
// whose DMA hasn't been submitted yet → s_slot_done is set and slot is freed
// while the real DMA is still in flight → subsequent allocator reuses the
// slot, overwrites the PRDT, and DMA writes data to the wrong buffer.
static void ahci_rescan_completions(void) {
    if (!s_port) return;
    // Same preempt-disable rationale as ahci_irq_handler: this runs from
    // sched_tick (timer IRQ) AND from slot_wait (process context).  Either
    // way, wait_queue_wake_all → rcu_read_unlock → preempt_enable could
    // otherwise call sched_preempt → do_switch → `sti` inside an ISR and
    // let another IRQ nest.  Works for the process-context caller too:
    // bump and dec is cheap, and the caller's own preempt_enable (if
    // any) still reaches zero eventually.
    preempt_disable();
    for (;;) {
        // irqsave: callable from IRQ context (sched_tick path) AND
        // process context (slot_wait hook).  The lock is also taken by
        // issue_cmd in process context; a timer IRQ firing on the CPU
        // that holds it would self-deadlock without disabling IRQs.
        uint64_t flags = spin_lock_irqsave(&s_ci_lock);
        uint32_t active = __atomic_load_n(&s_active_mask, __ATOMIC_ACQUIRE);
        if (!active) {
            spin_unlock_irqrestore(&s_ci_lock, flags);
            this_cpu()->preempt_depth--;
            return;
        }
        uint32_t done = s_ncq ? (active & ~s_port->sact) : (active & ~s_port->ci);
        if (!done) {
            spin_unlock_irqrestore(&s_ci_lock, flags);
            this_cpu()->preempt_depth--;
            return;
        }
        __atomic_fetch_and(&s_active_mask, ~done, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&s_ci_lock, flags);

        uint32_t mask = done;
        while (mask) {
            uint32_t s = (uint32_t)__builtin_ctz(mask);
            mask &= mask - 1u;
            s_slot_result[s] = 1;
            __atomic_store_n(&s_slot_done[s], 1u, __ATOMIC_RELEASE);
            wait_queue_wake_all(&s_slot_wq[s]);
        }
        __atomic_fetch_or(&s_free_mask, done, __ATOMIC_RELEASE);
        wait_queue_wake_all(&s_slot_avail_wq);
    }
}

static void slot_wait(uint32_t slot) {
    // Post-wake hook: rescan for completions the ISR may have missed due
    // to IRQ coalescing (W1C of port_is can clear a completion's IS bit
    // before the ISR reads SACT/CI).
    WAIT_EVENT_HOOK(&s_slot_wq[slot],
                    __atomic_load_n(&s_slot_done[slot], __ATOMIC_ACQUIRE),
                    ahci_rescan_completions());
}

// ── Pre-scheduler polling path ────────────────────────────────────────────
// Slot 0, per-slot command table, direct zero-copy DMA to destination.
// Runs single-threaded (BSP only, interrupts off) before sched_init().
// PRDT entries are built page-by-page so physically-discontiguous buffers
// (stack, BSS, HHDM heap) are all handled correctly.

static uint8_t do_rw_direct(uint64_t lba, void* buf, uint32_t count,
                             uint8_t write) {
    if (!s_port) return 0;
    hba_port_t* p = s_port;

    while (count > 0) {
        // ATA sector count field is 16 bits; cap at 65535 (0 means 65536).
        uint32_t n     = (count < 0xFFFFu) ? count : 0xFFFFu;
        uint32_t bytes = n * 512u;

        // Wait for port idle.
        for (int i = 0; i < 100000; i++) {
            if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) break;
            if (i == 99999) return 0;
            udelay(10);
        }

        // Build PRDT page-by-page so physical discontinuities are handled.
        cmd_header_t* hdr = (cmd_header_t*)(s_cmdlist_phys + HHDM_OFFSET);
        cmd_table_t*  ct  = (cmd_table_t*)(s_ctbl_phys[0] + HHDM_OFFSET);

        uint8_t*  p8      = (uint8_t*)buf;
        uint32_t  rem     = bytes;
        uint16_t  nprdt   = 0;
        while (rem > 0 && nprdt < 248u) {
            uint32_t pg_off = (uint32_t)((uint64_t)p8 & (PAGE_SIZE - 1u));
            uint32_t chunk  = PAGE_SIZE - pg_off;
            if (chunk > rem)     chunk = rem;
            // DBC field: bits[21:0], value = byte_count - 1; max 4MB - 2.
            if (chunk > 0x3FFFFEu) chunk = 0x3FFFFEu;
            phys_addr_t pa = va_to_phys(p8);
            ct->prdt[nprdt].dba  = (uint32_t)(pa & 0xFFFFFFFFu);
            ct->prdt[nprdt].dbau = (uint32_t)(pa >> 32);
            ct->prdt[nprdt].rsv  = 0;
            ct->prdt[nprdt].dbc  = chunk - 1u;
            p8  += chunk;
            rem -= chunk;
            nprdt++;
        }
        if (rem) return 0;   // buffer too fragmented (should never happen)

        hdr[0].flags = 5u | (write ? (1u << 6) : 0u);
        hdr[0].prdtl = nprdt;
        hdr[0].prdbc = 0;

        // Build H2D Register FIS.
        uint8_t* fis = ct->cfis;
        __builtin_memset(fis, 0, 64);
        fis[0]  = FIS_TYPE_H2D;
        fis[1]  = 0x80;   // C=1 (command)
        fis[2]  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        fis[4]  = (uint8_t)(lba         & 0xFFu);
        fis[5]  = (uint8_t)((lba >>  8) & 0xFFu);
        fis[6]  = (uint8_t)((lba >> 16) & 0xFFu);
        fis[7]  = 0x40;   // LBA mode
        fis[8]  = (uint8_t)((lba >> 24) & 0xFFu);
        fis[9]  = (uint8_t)((lba >> 32) & 0xFFu);
        fis[10] = (uint8_t)((lba >> 40) & 0xFFu);
        fis[12] = (uint8_t)(n           & 0xFFu);
        fis[13] = (uint8_t)((n >>    8) & 0xFFu);

        p->is   = p->is;    // W1C: clear pending interrupts
        p->serr = p->serr;
        p->ci   = 1u;       // slot 0

        // Busy-poll until slot 0 clears from CI.
        for (int i = 0; i < 3000000; i++) {
            if (!(p->ci & 1u)) break;
            if (p->is & PORT_IS_TFES) return 0;
            if (i == 2999999)  return 0;
            udelay(10);
        }
        if (p->is & PORT_IS_TFES) return 0;

        buf    = (uint8_t*)buf + bytes;
        lba   += n;
        count -= n;
    }
    return 1;
}

// ── NCQ submit helpers ────────────────────────────────────────────────────

// Submit one NCQ/DMA command for any kernel virtual buffer.
// Handles three kernel VA ranges:
//   HHDM  [HHDM_OFFSET, 0xFFFFFFFF80000000) → phys = va - HHDM_OFFSET  (fast)
//   All others (kernel stacks, BSS, etc.)    → vmm_page_phys page-table walk
// PRDT is built page-by-page so physically-discontiguous mappings
// (e.g. kernel stacks allocated as individual PMM pages) are always correct.

// Poison sentinel: written to first 8 bytes of read buffer before DMA.
// If DMA completes "successfully" but didn't overwrite it, we know the
// read silently failed (QEMU AHCI quirk where PRDBC is not updated).
// Retry up to AHCI_MAX_RETRIES times before giving up.
#define AHCI_READ_POISON  0xDEAD5A5ADEADBEEFULL
#define AHCI_MAX_RETRIES  5

// Serialize ahci_submit_hhdm across all submitters.
//
// With s_nslots=1 (non-NCQ fallback for QEMU), slot_alloc's CAS already
// ensures exclusive slot ownership.  But the post-slot_wait checks of
// s_slot_result[slot] race against the NEXT submitter's reset of that
// same array (B: `s_slot_result[slot] = 0` before issue), which can
// cause A to spuriously retry and, in concert with the retry-path
// PRDT rebuild, opens a window where concurrent DMA targets cross-
// contaminate buffers.  Full serialization eliminates the race and is
// zero cost at s_nslots=1 (submits were already effectively serial).
// When NCQ is re-enabled with s_nslots>1, replace this with per-slot
// state that's not reused by later submitters.
static volatile uint32_t s_submit_busy = 0;
static wait_queue_t      s_submit_busy_wq = { NULL, SPINLOCK_INIT };

static uint8_t ahci_submit_hhdm(uint64_t lba, void* buf, uint32_t count,
                                  uint8_t write) {
    if (!buf || !count) return 0;

    uint32_t bytes = count * 512u;

    // Acquire exclusive submit: spin-CAS with sleep-fallback.
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&s_submit_busy, &expected, 1u, 0,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        WAIT_EVENT(&s_submit_busy_wq,
                   __atomic_load_n(&s_submit_busy, __ATOMIC_ACQUIRE) == 0);
    }

    uint8_t rc = 0;

    for (uint32_t attempt = 0; attempt < AHCI_MAX_RETRIES; attempt++) {
        uint32_t slot = slot_alloc();
        cmd_table_t* ct = (cmd_table_t*)(s_ctbl_phys[slot] + HHDM_OFFSET);

        uint8_t*    p8    = (uint8_t*)buf;
        uint32_t    rem   = bytes;
        uint16_t    nprdt = 0;
        phys_addr_t kpml4 = 0;

        while (rem > 0 && nprdt < 248u) {
            uint32_t pg_off = (uint32_t)((uint64_t)p8 & (PAGE_SIZE - 1u));
            uint32_t chunk  = PAGE_SIZE - pg_off;
            if (chunk > rem)      chunk = rem;
            if (chunk > 0x3FFFFEu) chunk = 0x3FFFFEu;

            uint64_t  v = (uint64_t)p8;
            phys_addr_t pa;
            if (v >= HHDM_OFFSET && v < 0xFFFFFFFF80000000ULL) {
                pa = v - HHDM_OFFSET;
            } else {
                if (!kpml4) kpml4 = vmm_kernel_pml4_get();
                pa = vmm_page_phys(kpml4, v & ~(uint64_t)(PAGE_SIZE - 1u)) + pg_off;
            }

            ct->prdt[nprdt].dba  = (uint32_t)(pa & 0xFFFFFFFFu);
            ct->prdt[nprdt].dbau = (uint32_t)(pa >> 32);
            ct->prdt[nprdt].rsv  = 0;
            ct->prdt[nprdt].dbc  = chunk - 1u;
            p8  += chunk;
            rem -= chunk;
            nprdt++;
        }

        volatile uint64_t* sentinel = (volatile uint64_t*)buf;
        if (!write && bytes >= 8)
            *sentinel = AHCI_READ_POISON;

        s_slot_done[slot]   = 0;
        s_slot_result[slot] = 0;
        issue_cmd(slot, lba, count, write, nprdt);
        slot_wait(slot);

        if (!s_slot_result[slot])
            continue;  // HBA error — retry

        // If sentinel survived, DMA didn't deliver data — retry.
        if (!write && bytes >= 8 && *sentinel == AHCI_READ_POISON)
            continue;

        rc = 1;  // success
        break;
    }

    __atomic_store_n(&s_submit_busy, 0u, __ATOMIC_RELEASE);
    wait_queue_wake_all(&s_submit_busy_wq);
    return rc;
}

// Submit using a pre-resolved scatter-gather page list (user-space path).
// pages[i]: HHDM kernel pointer to the start of each 4 KiB user page.
// first_off: byte offset within pages[0].

static uint8_t ahci_submit_sg(uint64_t lba, uint8_t** pages, uint32_t npages,
                                uint32_t first_off, uint32_t count,
                                uint8_t write) {
    phys_addr_t phys_pages[130];
    for (uint32_t i = 0; i < npages; i++)
        phys_pages[i] = (phys_addr_t)((uint64_t)pages[i] - HHDM_OFFSET);

    uint32_t bytes = count * 512u;
    uint32_t slot  = slot_alloc();

    cmd_table_t* ct = (cmd_table_t*)(s_ctbl_phys[slot] + HHDM_OFFSET);
    uint16_t nprdt = (uint16_t)build_prdt_pages(ct->prdt, phys_pages, npages,
                                                  first_off, bytes);

    s_slot_done[slot]   = 0;
    s_slot_result[slot] = 0;
    issue_cmd(slot, lba, count, write, nprdt);
    slot_wait(slot);
    return s_slot_result[slot];
}

// ── Public API ────────────────────────────────────────────────────────────

uint8_t ahci_read(uint64_t lba, void* buf, uint32_t count) {
    if (!s_port) return 0;
    if (s_irq_ready) return ahci_submit_hhdm(lba, buf, count, 0);
    return do_rw_direct(lba, buf, count, 0);
}

uint8_t ahci_write(uint64_t lba, const void* buf, uint32_t count) {
    if (!s_port) return 0;
    if (s_irq_ready) return ahci_submit_hhdm(lba, (void*)buf, count, 1);
    return do_rw_direct(lba, (void*)buf, count, 1);
}

// Read directly into a user-space buffer (zero-copy scatter-gather).
// Resolves user virtual pages to physical frames via page tables,
// then builds a multi-entry PRDT so the HBA DMA's into user memory directly.
uint8_t ahci_read_user(uint64_t lba, void* user_buf, uint32_t count) {
    if (!s_port) return 0;
    if (!s_irq_ready) return do_rw_direct(lba, user_buf, count, 0);

    uint64_t va        = (uint64_t)user_buf;
    uint32_t total_bytes = count * 512u;
    uint32_t first_off = (uint32_t)(va & 0xFFFu);
    uint32_t npages    = (first_off + total_bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

    if (npages > 130u) return 0;

    phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
    void* page_ptrs[130];
    if (!vmm_get_user_pages(pml4, va, npages, page_ptrs)) return 0;

    // vmm_get_user_pages already pinned each frame UNDER its resolution
    // lock (closing the resolve→DMA recycle window).  We own those pins;
    // release them after the DMA completes.
    phys_addr_t pin_addrs[130];
    for (uint32_t i = 0; i < npages; i++)
        pin_addrs[i] = (phys_addr_t)((uint64_t)page_ptrs[i] - HHDM_OFFSET);

    uint8_t ok = ahci_submit_sg(lba, (uint8_t**)page_ptrs, npages,
                                  first_off, count, 0);

    for (uint32_t i = 0; i < npages; i++)
        pmm_unpin(pin_addrs[i]);

    return ok;
}

// Parallel multi-page read into PMM-allocated frames.
//
// Submits `nframes` independent NCQ read commands simultaneously (all in-flight
// at once), then waits for each to complete.  With NCQ the HBA can re-order and
// pipeline the reads for optimal rotational latency — especially useful for
// read-ahead where all target pages are known upfront.
//
// frames[i]:  physical address of a pre-allocated 4 KiB PMM frame (must be from
//             pmm_buddy_alloc(0)), or PMM_INVALID_ADDR to skip slot i.
// lba:        first LBA; each page reads 8 sectors (lba + i*8).
// nframes:    1..32 (capped at MAX_NCQ_SLOTS).
//
// Returns a bitmask: bit i = 1 if frames[i] was successfully filled.
// Caller owns all frames regardless of success — free on failure.
uint32_t ahci_read_multi(uint64_t lba, phys_addr_t* frames, uint32_t nframes) {
    if (!s_port || !s_irq_ready || !nframes) return 0;
    if (nframes > MAX_NCQ_SLOTS) nframes = MAX_NCQ_SLOTS;

    uint32_t slots[MAX_NCQ_SLOTS];
    uint32_t submitted_mask = 0;

    // Submit all reads before waiting for any (maximises HBA parallelism).
    for (uint32_t i = 0; i < nframes; i++) {
        if (frames[i] == PMM_INVALID_ADDR) { slots[i] = 0xFFu; continue; }

        uint32_t slot = slot_alloc();
        slots[i] = slot;

        cmd_table_t* ct = (cmd_table_t*)(s_ctbl_phys[slot] + HHDM_OFFSET);
        uint16_t nprdt = (uint16_t)build_prdt(ct->prdt, frames[i], PAGE_SIZE);

        // Poison first 8 bytes so we can detect zero-data DMA on collect.
        *(volatile uint64_t*)(frames[i] + HHDM_OFFSET) = AHCI_READ_POISON;

        s_slot_done[slot]   = 0;
        s_slot_result[slot] = 0;
        issue_cmd(slot, lba + (uint64_t)i * (PAGE_SIZE / 512u),
                  PAGE_SIZE / 512u, 0, nprdt);
        submitted_mask |= (1u << i);
    }

    // Now collect results.
    uint32_t result_mask = 0;
    for (uint32_t i = 0; i < nframes; i++) {
        if (!(submitted_mask & (1u << i))) continue;
        slot_wait(slots[i]);
        if (!s_slot_result[slots[i]]) continue;
        // Verify DMA actually delivered data (sentinel check).
        volatile uint64_t* p = (volatile uint64_t*)(frames[i] + HHDM_OFFSET);
        if (*p == AHCI_READ_POISON) continue;  // zero-data DMA — skip
        result_mask |= (1u << i);
    }
    return result_mask;
}

// ── Start IRQ path (call after sched_init) ────────────────────────────────

void ahci_poll_completions(void) {
    if (!s_irq_ready) return;
    ahci_rescan_completions();
}

void ahci_start_io_thread(void) {
    // In the NCQ design there is no dedicated I/O kthread.  Any CPU that
    // calls ahci_read/ahci_write allocates a free command slot, issues the
    // NCQ command, and sleeps on its per-slot wait queue until the IRQ
    // handler wakes it.  This function just arms the IRQ path.
    if (g_ahci_irq == 0xFF) return;  // no MSI/MSI-X — stay in polling mode

    // Drain stale port_is / hba_is left by polling-mode reads.
    // MSI-X is edge-triggered (fires on 0→1 IS transition).  If DHRS is
    // already set from the last do_rw_direct completion, the first IRQ-path
    // command's completion sets DHRS again → no edge → no MSI → slot_wait
    // sleeps forever.  W1C both registers so the first real IRQ fires.
    if (s_port) {
        s_port->is  = s_port->is;   // W1C all pending port interrupt bits
        s_port->serr = s_port->serr; // W1C any stale errors
    }
    if (s_hba)
        s_hba->is = s_hba->is;      // W1C global IS

    s_irq_ready = 1;
}

// ── ahci_init ─────────────────────────────────────────────────────────────

uint8_t ahci_init(void) {
    // 1. Locate AHCI controller.
    pci_device_t dev;
    if (!pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_AHCI, &dev)) return 0;

    // 2. Enable MMIO + bus mastering.
    pci_enable(dev.bus, dev.dev, dev.fn);

    // 3. Map ABAR (BAR5).
    uint64_t abar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn, AHCI_BAR);
    if (!abar_phys) return 0;
    s_hba = (hba_mem_t*)vmm_map_mmio(abar_phys, sizeof(hba_mem_t));

    // 4. Enable AHCI mode.
    s_hba->ghc |= HBA_GHC_AE;

    // 5. Force non-NCQ mode regardless of what HBA CAP reports.
    //
    // QEMU's AHCI NCQ (FPDMA) AIO backend intermittently stalls: CI is
    // cleared (QEMU fetched the command descriptor) but the Set Device Bits
    // FIS is never sent, so SACT stays set and slot_wait sleeps forever.
    // This manifests as ~1/6 boots where login's first disk read never
    // completes.
    //
    // Non-NCQ READ/WRITE DMA EXT (0x25/0x35) are rock-solid under QEMU:
    // the HBA clears CI directly when it receives the D2H FIS, no device
    // FIS round-trip needed.  We lose nothing — there is no rotational-seek
    // benefit to NCQ on a virtual disk, and s_nslots=1 is already the
    // effective limit when NCQ is disabled.
    s_ncq    = 0u;
    s_nslots = 1u;

    // 6. Find the first SATA port with a drive.
    uint32_t pi = s_hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t* p = &s_hba->ports[i];
        if ((p->ssts & 0xFu) != PORT_SSTS_DET_PRESENT) continue;
        if (p->sig != SATA_SIG_ATA) continue;

        s_port = p;
        port_init(p);

        // 7. Enable global HBA interrupt delivery.
        s_hba->ghc |= HBA_GHC_IE;

        // 8. Wire up the IRQ: try MSI-X first, fall back to MSI.
        //
        // MSI-X (cap 0x11):
        //   Programs the table entry with lowest-priority delivery so the
        //   interrupt is routed to whichever CPU currently has the lowest
        //   task priority — naturally distributing IRQ load without any
        //   per-request steering.
        //   Table entry format (16 bytes, MMIO):
        //     [0] msg_addr_lo  [1] msg_addr_hi  [2] msg_data  [3] vector_ctrl
        //
        // MSI addr for lowest-priority delivery (Intel SDM §10.11.1):
        //   bit[2]   = DM (destination mode 1 = logical addressing)
        //   bit[3]   = RH (redirection hint 1 = use lowest-priority arbitration)
        //   bits[19:12] = destination 0xFF (all CPUs in flat logical model)
        //   0xFEE00000 | (0xFF<<12) | (1<<3) | (1<<2) = 0xFEEFF00C
        //
        // MSI data for lowest-priority delivery (delivery mode 001):
        //   bits[10:8] = 001 → (1u<<8)
        //   bits[7:0]  = vector
        {
            uint8_t cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                                    0x34u) & 0xFCu);
            uint8_t msix_cap = 0, msi_cap = 0;
            while (cap) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
                uint8_t  id = dw & 0xFFu;
                if (id == 0x11u && !msix_cap) msix_cap = cap;
                if (id == 0x05u && !msi_cap)  msi_cap  = cap;
                cap = (uint8_t)((dw >> 8) & 0xFCu);
            }

            int irq_armed = 0;

            if (msix_cap) {
                // MSI-X: read table offset and BIR from cap+4.
                uint32_t tbl_dw  = pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                                    msix_cap + 4u);
                uint32_t bir     = tbl_dw & 0x7u;
                uint32_t tbl_off = tbl_dw & ~0x7u;
                uint64_t bar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn,
                                                  (uint8_t)bir);
                if (bar_phys) {
                    // Map just the 16-byte entry 0 (we only use one vector).
                    s_msix_entry = (volatile uint32_t*)
                                   vmm_map_mmio(bar_phys + tbl_off, 16u);

                    // Fixed delivery to BSP LAPIC — works in both xAPIC and
                    // x2APIC modes.  Lowest-priority (mode 001) is NOT
                    // supported in x2APIC and silently drops the interrupt.
                    uint32_t msi_addr = (uint32_t)lapic_msi_addr();
                    uint32_t msi_data = lapic_msi_data(VEC_AHCI_MSI);

                    s_msix_entry[0] = msi_addr;   // addr_lo
                    s_msix_entry[1] = 0;           // addr_hi
                    s_msix_entry[2] = msi_data;    // data
                    s_msix_entry[3] = 0;           // vector_ctrl: bit0=0 → unmasked

                    // Enable MSI-X in message control (bit 31).
                    // Also clear Function Mask (bit 30) so vectors fire.
                    uint32_t mc = pci_cfg_read32(dev.bus, dev.dev, dev.fn, msix_cap);
                    mc = (mc | (1u << 31)) & ~(1u << 30);
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, msix_cap, mc);

                    irq_armed = 1;
                }
            }

            if (!irq_armed && msi_cap) {
                // MSI fallback: Fixed delivery to BSP LAPIC.
                uint32_t mc      = pci_cfg_read32(dev.bus, dev.dev, dev.fn, msi_cap);
                int      is_64   = (mc >> 23) & 1;
                uint32_t msi_addr = (uint32_t)lapic_msi_addr();
                uint32_t msi_data = lapic_msi_data(VEC_AHCI_MSI);

                pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                msi_cap + 4u, msi_addr);
                if (is_64) {
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, msi_cap + 8u,  0u);
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, msi_cap + 12u, msi_data);
                } else {
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, msi_cap + 8u,  msi_data);
                }
                // Enable MSI (bit 16), single message (MME=000).
                pci_cfg_write32(dev.bus, dev.dev, dev.fn, msi_cap,
                                (mc & ~(0x7u << 20)) | (1u << 16));

                irq_armed = 1;
            }

            idt_irq_register(VEC_AHCI_MSI, (uint64_t)ahci_irq_entry);
            g_ahci_irq = irq_armed ? 11u : 0xFFu;
        }

        return 1;
    }

    return 0;
}
