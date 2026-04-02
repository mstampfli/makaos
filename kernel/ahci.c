#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "common.h"
#include "idt.h"
#include "pic.h"
#include "irq_wait.h"
#include "sched.h"

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
    for (int i = 0; i < 4096; i++) cl[i] = fb[i] = ct[i] = 0;
    uint8_t* dma = (uint8_t*)(s_dma_phys + HHDM_OFFSET);
    for (uint32_t i = 0; i < (1u << DMA_ORDER) * PAGE_SIZE; i++) dma[i] = 0;

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

static uint8_t issue_one(uint64_t lba, uint32_t count, uint8_t write) {
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
    for (int i = 0; i < 64; i++) fis[i] = 0;
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

    if (g_ahci_irq != 0xFF && g_current != NULL) {
        do {
            if (p->is & PORT_IS_TFES) return 0;
            irq_wait(g_ahci_irq);
        } while (p->ci & 1u);
    } else {
        // Polling fallback: used during early init (before scheduler).
        for (int i = 0; i < 3000000; i++) {
            if (!(p->ci & 1u)) break;
            if (p->is & PORT_IS_TFES) return 0;
            if (i == 2999999) return 0;
            udelay(10);
        }
    }
    return (p->is & PORT_IS_TFES) ? 0 : 1;
}

// ── Public read/write — uses bounce buffer, safe for any kernel pointer ───

static uint8_t do_rw(uint64_t lba, void* buf, uint32_t count, uint8_t write) {
    if (!s_port) return 0;
    uint8_t* p8 = (uint8_t*)buf;

    while (count > 0) {
        uint32_t n     = count < DMA_SECTORS ? count : DMA_SECTORS;
        uint32_t bytes = n * 512;
        uint8_t* dma   = (uint8_t*)(s_dma_phys + HHDM_OFFSET);

        if (write) {
            for (uint32_t i = 0; i < bytes; i++) dma[i] = p8[i];
        }

        if (!issue_one(lba, n, write)) return 0;

        if (!write) {
            for (uint32_t i = 0; i < bytes; i++) p8[i] = dma[i];
        }

        p8    += bytes;
        lba   += n;
        count -= n;
    }
    return 1;
}

// ── Public API ────────────────────────────────────────────────────────────

uint8_t ahci_read(uint64_t lba, void* buf, uint32_t count) {
    return do_rw(lba, buf, count, 0);
}

uint8_t ahci_write(uint64_t lba, const void* buf, uint32_t count) {
    return do_rw(lba, (void*)buf, count, 1);
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

        // 7. Program PIIX3 PIRQ routing and register the ISR.
        //
        // QEMU's PIIX3 ISA bridge (bus=0,dev=1,fn=0) has 4 PIRQ lines A-D
        // at config offsets 0x60-0x63.  They default to 0x80 (disabled).
        // We must program them ourselves (no BIOS present).
        //
        // i440fx routing formula: pirq_idx = (slot + intpin - 2) % 4
        //   where slot = PCI device number, intpin 1-based (INTA=1 … INTD=4)
        #define PIRQ_IRQ  11u          // route everything to IRQ 11
        #define PIIX3_BUS 0
        #define PIIX3_DEV 1
        #define PIIX3_FN  0
        {
            uint32_t cfg3c = pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x3C);
            uint8_t  int_pin = (uint8_t)((cfg3c >> 8) & 0xFF);
            if (int_pin == 0 || int_pin > 4) int_pin = 1;

            uint8_t pirq_idx = (uint8_t)((dev.dev + int_pin - 2u) % 4u);

            // Program PIRQA-D dword (offsets 0x60-0x63 share one dword).
            uint32_t pirq_dw = pci_cfg_read32(PIIX3_BUS, PIIX3_DEV, PIIX3_FN, 0x60);
            uint32_t shift = pirq_idx * 8u;
            pirq_dw = (pirq_dw & ~(0xFFu << shift)) | ((uint32_t)PIRQ_IRQ << shift);
            pci_cfg_write32(PIIX3_BUS, PIIX3_DEV, PIIX3_FN, 0x60, pirq_dw);

            // Update the device's Interrupt Line register so our code reads it back.
            pci_cfg_write32(dev.bus, dev.dev, dev.fn, 0x3C,
                            (cfg3c & ~0xFFu) | PIRQ_IRQ);

            g_ahci_irq = PIRQ_IRQ;
            uint8_t vec = (PIRQ_IRQ < 8u) ? (0x20u + PIRQ_IRQ)
                                           : (0x28u + PIRQ_IRQ - 8u);
            idt_irq_register(vec, (uint64_t)ahci_irq_entry);
            pic_unmask(PIRQ_IRQ);
        }

        return 1;
    }

    return 0;
}
