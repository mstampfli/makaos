#include "ahci_loader.h"
#include "common.h"

/* ── PCI ──────────────────────────────────────────────────────────────────── */
#define PCI_ADDR_PORT 0xCF8u
#define PCI_DATA_PORT 0xCFCu

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(fn & 0x7) << 8) | (off & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    return inl(PCI_DATA_PORT);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(fn & 0x7) << 8) | (off & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    outl(PCI_DATA_PORT, val);
}

/* Scan all buses for class=0x01, subclass=0x06 (AHCI). */
static uint8_t pci_find_ahci(uint8_t* b_out, uint8_t* d_out, uint8_t* f_out) {
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            /* Check function 0 first; skip device if not present. */
            if (pci_read32(b, d, 0, 0) == 0xFFFFFFFF) continue;
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0x00);
                if (id == 0xFFFFFFFF) { if (f == 0) break; continue; }
                uint32_t cc = pci_read32(b, d, f, 0x08);
                if (((cc >> 24) & 0xFF) == 0x01 && ((cc >> 16) & 0xFF) == 0x06) {
                    *b_out = (uint8_t)b; *d_out = d; *f_out = f;
                    return 1;
                }
                if (f == 0 && !((pci_read32(b, d, 0, 0x0C) >> 16) & 0x80)) break;
            }
        }
    }
    return 0;
}

/* Enable MMIO + bus master. */
static void pci_enable(uint8_t b, uint8_t d, uint8_t f) {
    uint32_t cmd = pci_read32(b, d, f, 0x04);
    cmd |= (1u << 1) | (1u << 2); /* MMIO enable, bus master */
    pci_write32(b, d, f, 0x04, cmd);
}

/* Read 64-bit BAR (handles 32/64-bit variants). */
static uint64_t pci_bar_base(uint8_t b, uint8_t d, uint8_t f, uint8_t bar) {
    uint8_t off = 0x10 + bar * 4;
    uint32_t lo = pci_read32(b, d, f, off);
    if (lo & 1) return 0; /* I/O bar, not MMIO */
    uint32_t type = (lo >> 1) & 3;
    lo &= ~0xFu;
    if (type == 2) { /* 64-bit */
        uint64_t hi = pci_read32(b, d, f, off + 4);
        return lo | (hi << 32);
    }
    return lo;
}

/* ── HBA structures ───────────────────────────────────────────────────────── */
#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)
#define PORT_SSTS_DET_PRESENT 0x3u
#define PORT_TFD_BSY  (1u << 7)
#define PORT_TFD_DRQ  (1u << 3)
#define PORT_IS_TFES  (1u << 30)
#define SATA_SIG_ATA  0x00000101u
#define FIS_TYPE_H2D  0x27u
#define ATA_CMD_READ_DMA_EXT 0x25u
#define HBA_GHC_AE    (1u << 31)

typedef volatile struct {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, rsv0;
    uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) cmd_header_t;

typedef struct {
    uint32_t dba, dbau, rsv, dbc;
} __attribute__((packed)) prdt_entry_t;

typedef struct {
    uint8_t      cfis[64];
    uint8_t      acmd[16];
    uint8_t      rsv[48];
    prdt_entry_t prdt[1];
} __attribute__((packed)) cmd_table_t;

/* ── Static DMA buffers (loader is identity-mapped → phys == virt) ───────── */
static uint8_t s_cmdlist[4096] __attribute__((aligned(4096)));
static uint8_t s_fisrx  [4096] __attribute__((aligned(4096)));
static uint8_t s_cmdtbl [4096] __attribute__((aligned(4096)));

/* Bounce buffer: 64 sectors = 32 KiB (8 pages). */
#define DMA_SECTORS 64u
static uint8_t s_dma[DMA_SECTORS * 512] __attribute__((aligned(4096)));

static hba_port_t* s_port = NULL;

/* ── Port stop/start ─────────────────────────────────────────────────────── */
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

/* ── Issue one READ DMA EXT via polling ──────────────────────────────────── */
static uint8_t issue_read(uint64_t lba, uint32_t count) {
    hba_port_t* p = s_port;

    /* Wait for idle. */
    for (int i = 0; i < 100000; i++) {
        if (!(p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ))) break;
        if (i == 99999) return 0;
        udelay(10);
    }

    uint32_t bytes = count * 512;

    cmd_header_t* hdr = (cmd_header_t*)s_cmdlist;
    hdr[0].flags = 5;          /* CFL=5 (H2D FIS length / 4) */
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    cmd_table_t* ct = (cmd_table_t*)s_cmdtbl;
    ct->prdt[0].dba  = (uint32_t)((uint64_t)s_dma & 0xFFFFFFFF);
    ct->prdt[0].dbau = (uint32_t)((uint64_t)s_dma >> 32);
    ct->prdt[0].rsv  = 0;
    ct->prdt[0].dbc  = (bytes - 1) & 0x3FFFFF;

    uint8_t* fis = ct->cfis;
    for (int i = 0; i < 64; i++) fis[i] = 0;
    fis[0]  = FIS_TYPE_H2D;
    fis[1]  = 0x80;                       /* C=1: command register */
    fis[2]  = ATA_CMD_READ_DMA_EXT;
    fis[4]  = (uint8_t)(lba        & 0xFF);
    fis[5]  = (uint8_t)((lba >>  8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = 0x40;                       /* LBA mode */
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[12] = (uint8_t)(count       & 0xFF);
    fis[13] = (uint8_t)((count >> 8) & 0xFF);

    p->is   = p->is;   /* W1C */
    p->serr = p->serr;
    p->ci   = 1u;

    for (int i = 0; i < 3000000; i++) {
        if (!(p->ci & 1u)) break;
        if (p->is & PORT_IS_TFES) return 0;
        if (i == 2999999) return 0;
        udelay(10);
    }
    return (p->is & PORT_IS_TFES) ? 0 : 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

uint8_t ahci_loader_init(void) {
    uint8_t bus, dev, fn;
    if (!pci_find_ahci(&bus, &dev, &fn)) return 0;

    pci_enable(bus, dev, fn);

    uint64_t abar_phys = pci_bar_base(bus, dev, fn, 5);
    if (!abar_phys) return 0;

    /* ABAR is identity-mapped via the PDPT[3] 1GiB huge page added in main.c. */
    hba_mem_t* hba = (hba_mem_t*)abar_phys;

    hba->ghc |= HBA_GHC_AE;

    uint32_t pi = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t* p = &hba->ports[i];
        if ((p->ssts & 0xF) != PORT_SSTS_DET_PRESENT) continue;
        if (p->sig != SATA_SIG_ATA) continue;

        port_stop(p);

        /* Zero DMA buffers. */
        for (int j = 0; j < 4096; j++) s_cmdlist[j] = s_fisrx[j] = s_cmdtbl[j] = 0;
        for (uint32_t j = 0; j < DMA_SECTORS * 512; j++) s_dma[j] = 0;

        /* Wire up command header 0 → command table. */
        cmd_header_t* hdr = (cmd_header_t*)s_cmdlist;
        hdr[0].ctba  = (uint32_t)((uint64_t)s_cmdtbl & 0xFFFFFFFF);
        hdr[0].ctbau = (uint32_t)((uint64_t)s_cmdtbl >> 32);

        /* Program port registers. */
        p->clb  = (uint32_t)((uint64_t)s_cmdlist & 0xFFFFFFFF);
        p->clbu = (uint32_t)((uint64_t)s_cmdlist >> 32);
        p->fb   = (uint32_t)((uint64_t)s_fisrx & 0xFFFFFFFF);
        p->fbu  = (uint32_t)((uint64_t)s_fisrx >> 32);
        p->is   = p->is;
        p->serr = p->serr;

        port_start(p);
        s_port = p;
        return 1;
    }
    return 0;
}

uint8_t ahci_loader_read(uint64_t lba, void* buf, uint32_t count) {
    if (!s_port) return 0;
    uint8_t* dst = (uint8_t*)buf;

    while (count > 0) {
        uint32_t n     = count < DMA_SECTORS ? count : DMA_SECTORS;
        uint32_t bytes = n * 512;

        if (!issue_read(lba, n)) return 0;

        /* Copy from bounce buffer to destination. */
        for (uint32_t i = 0; i < bytes; i++) dst[i] = s_dma[i];

        dst   += bytes;
        lba   += n;
        count -= n;
    }
    return 1;
}
