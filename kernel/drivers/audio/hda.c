#include "hda.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "irq_wait.h"
#include "idt.h"
#include "lapic.h"
#include "common.h"
#include "smp.h"

extern void hda_irq_entry(void);

// ── PCI ───────────────────────────────────────────────────────────────────
#define PCI_CLASS_MULTIMEDIA  0x04u
#define PCI_SUBCLASS_HDA      0x03u

// ── HDA MMIO offsets ──────────────────────────────────────────────────────
#define HDA_GCAP      0x00u
#define HDA_GCTL      0x08u
#define HDA_STATESTS  0x0Eu
#define HDA_INTCTL    0x20u
#define HDA_INTSTS    0x24u
#define HDA_CORBBASE  0x40u
#define HDA_CORBWP    0x48u
#define HDA_CORBRP    0x4Au
#define HDA_CORBCTL   0x4Cu
#define HDA_CORBSIZE  0x4Eu
#define HDA_RIRBBASE  0x50u
#define HDA_RIRBWP    0x58u
#define HDA_RINTCNT   0x5Au
#define HDA_RIRBCTL   0x5Cu
#define HDA_RIRBSTS   0x5Du
#define HDA_RIRBSIZE  0x5Eu

// GCTL
#define GCTL_CRST   (1u << 0)
#define GCTL_UNSOL  (1u << 8)

// INTCTL
#define INTCTL_GIE  (1u << 31)

// CORB/RIRB
#define CORBCTL_RUN    (1u << 1)
#define CORBRP_RST     (1u << 15)
#define RIRBCTL_RUN    (1u << 1)
#define RIRBCTL_RINTFL (1u << 0)
#define RIRBSTS_RINTFL (1u << 0)

// ── Stream Descriptor offsets ─────────────────────────────────────────────
#define SD_CTL   0x00u
#define SD_STS   0x03u
#define SD_LPIB  0x04u
#define SD_CBL   0x08u
#define SD_LVI   0x0Cu
#define SD_FMT   0x12u
#define SD_BDPL  0x18u
#define SD_BDPU  0x1Cu

// SD_CTL bits
#define SD_CTL_SRST  (1u << 0)
#define SD_CTL_RUN   (1u << 1)
#define SD_CTL_IOCE  (1u << 2)
#define SD_CTL_FEIE  (1u << 3)
#define SD_CTL_DEIE  (1u << 4)
#define SD_CTL_TAG(t) ((uint32_t)(t) << 20)

// SD_STS bits (W1C)
#define SD_STS_BCIS  (1u << 2)
#define SD_STS_FIFOE (1u << 3)
#define SD_STS_DESE  (1u << 4)

// Stream format: 48kHz, 16-bit, stereo
// base=48kHz(0), mult=×1(0), div=÷1(0), bits=16(1), ch=stereo(1)
#define FMT_48K_16BIT_STEREO  0x0011u

// ── Verb helpers ──────────────────────────────────────────────────────────
#define VERB12(cad,nid,v12,pay) \
    (((uint32_t)(cad)<<28)|((uint32_t)(nid)<<20)|((uint32_t)(v12)<<8)|(uint32_t)(pay))
#define VERB4(cad,nid,v4,pay16) \
    (((uint32_t)(cad)<<28)|((uint32_t)(nid)<<20)|((uint32_t)(v4)<<16)|(uint32_t)(pay16))

#define V_GET_PARAM    0xF00u
#define V_SET_STRM     0x706u
#define V_SET_PIN_CTL  0x707u
#define V_SET_POWER    0x705u
#define V_SET_CONN_SEL 0x701u
#define V4_SET_FMT     0x2u
#define V4_SET_AMP     0x3u

#define PAR_NODE_COUNT  0x04u
#define PAR_FUNC_TYPE   0x05u
#define PAR_WIDGET_CAP  0x09u
#define PAR_PIN_CAP     0x0Cu

#define WTYPE_AUDIO_OUT 0x0u
#define WTYPE_PIN       0x4u

#define PIN_OUT_EN  (1u << 6)
#define PIN_HP_EN   (1u << 7)

// Output amp: set output, both channels, unmute, gain=0x27
#define AMP_OUT_BOTH_UNMUTE  0xB027u

// ── DMA ring parameters ───────────────────────────────────────────────────
//
// Architecture (identical to Linux HDA / Windows PortCls):
//   • Hardware runs a cyclic BDL ring forever with IOC on every entry.
//   • Each entry is BUF_BYTES of PCM = one "period".
//   • IRQ fires once per period.  Handler drains BUF_BYTES from the software
//     FIFO into the buffer hardware JUST finished (it is idle until the ring
//     wraps back to it).
//   • hda_write() is the application-facing API: it copies into the FIFO and
//     returns.  It never touches DMA buffers directly.
//   • Latency ≈ BUF_BYTES / (48000×2ch×2B).  At 256 B that is ~1.3 ms.
//     IRQ fires ~740×/sec via MSI → LAPIC, bypassing PIC routing entirely.
//
// BUF_BYTES must be a multiple of 4 (frame size) and ≥ 64.
// BDL_ENTRIES must be a power of 2 ≥ 2.

#define BDL_ENTRIES  8u          // 8 × 256 B = 2 KiB cyclic ring
#define BUF_BYTES    256u        // ~1.3 ms per period
#define BUF_ORDER    0u          // 2^0 pages = 4 KiB phys alloc per slot

// ── Software FIFO ─────────────────────────────────────────────────────────
// Ring buffer between application and DMA engine.  Must be larger than the
// largest single hda_write() call to avoid blocking mid-write.
// Doom writes ~5488 B per tic.  Keep FIFO_BYTES ≫ that.

#define FIFO_BYTES  8192u        // 8 KiB — larger than one doom tic (5488 B)
#define FIFO_MASK   (FIFO_BYTES - 1u)

// BDL entry
typedef struct {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t flags;     // bit0 = IOC
} __attribute__((packed)) bdle_t;

// CORB/RIRB ring size
#define RING_SZ  256u

// ── IRQ slot for irq_wait/irq_notify ─────────────────────────────────────
// HDA uses MSI (no PIC, no IOAPIC).  We keep a logical slot index so the
// irq_wait/irq_notify mechanism still works (it is slot-indexed, 0–15).
// Slot 5 is free (old PIC IRQ5 = sound card, now unused).
#define HDA_IRQ_SLOT  5u

// ── Driver state ──────────────────────────────────────────────────────────

uint8_t g_hda_irq = 0xFFu;

static int      s_ok      = 0;
static int      s_running = 0;
static uint8_t* s_mmio    = NULL;
static uint32_t s_sd_base = 0;
static uint8_t  s_iss     = 0;   // number of input streams (used for INTCTL bit)

static uint32_t* s_corb    = NULL;
static uint64_t* s_rirb    = NULL;
static uint16_t  s_rirb_rp = 0;

static bdle_t*  s_bdl      = NULL;
static uint8_t* s_buf[BDL_ENTRIES];

// Software FIFO — producer: hda_write(), consumer: IRQ handler
static uint8_t*          s_fifo    = NULL;
static volatile uint32_t s_fifo_wp = 0;   // written by hda_write()
// /dev/audio is multi-open (vfs); two writers on different CPUs would
// race s_fifo_wp and the lazy-start block.  Producer-side lock only —
// the ISR consumer (SPSC reader) never takes it.
static spinlock_t s_hda_wlock = SPINLOCK_INIT;
static volatile uint32_t s_fifo_rp = 0;   // written by IRQ handler

// Which DMA slot to fill next (advanced by IRQ handler)
static volatile uint8_t s_dma_wi = 0;

// ── MMIO helpers ──────────────────────────────────────────────────────────

static inline uint8_t  r8 (uint32_t o){ return *(volatile uint8_t* )(s_mmio+o); }
static inline uint16_t r16(uint32_t o){ return *(volatile uint16_t*)(s_mmio+o); }
static inline uint32_t r32(uint32_t o){ return *(volatile uint32_t*)(s_mmio+o); }
static inline void w8 (uint32_t o,uint8_t  v){ *(volatile uint8_t* )(s_mmio+o)=v; }
static inline void w16(uint32_t o,uint16_t v){ *(volatile uint16_t*)(s_mmio+o)=v; }
static inline void w32(uint32_t o,uint32_t v){ *(volatile uint32_t*)(s_mmio+o)=v; }

static inline uint32_t sd_r32(uint32_t r){ return r32(s_sd_base+r); }
static inline void sd_w8 (uint32_t r,uint8_t  v){ w8 (s_sd_base+r,v); }
static inline void sd_w16(uint32_t r,uint16_t v){ w16(s_sd_base+r,v); }
static inline void sd_w32(uint32_t r,uint32_t v){ w32(s_sd_base+r,v); }

// ── CORB/RIRB verb interface ──────────────────────────────────────────────

static uint32_t verb_send(uint32_t verb) {
    uint16_t wp = (uint16_t)((r16(HDA_CORBWP) + 1u) & (RING_SZ - 1u));
    s_corb[wp] = verb;
    __asm__ volatile("mfence" ::: "memory");
    w16(HDA_CORBWP, wp);

    uint16_t exp = (uint16_t)((s_rirb_rp + 1u) & (RING_SZ - 1u));
    for (int i = 0; i < 500000; i++) {
        __asm__ volatile("lfence" ::: "memory");
        if (r16(HDA_RIRBWP) == exp) break;
    }
    uint64_t resp = s_rirb[exp];
    s_rirb_rp = exp;
    w8(HDA_RIRBSTS, RIRBSTS_RINTFL);
    return (uint32_t)(resp & 0xFFFFFFFFu);
}

static uint32_t get_param(uint8_t cad, uint8_t nid, uint8_t par) {
    return verb_send(VERB12(cad, nid, V_GET_PARAM, par));
}

// ── Codec enumeration and configuration ──────────────────────────────────
// Walk the widget graph, find the first DAC→pin path, configure for
// 48 kHz / 16-bit / stereo output on stream tag 1.

static int codec_configure(uint8_t cad) {
    uint32_t nc      = get_param(cad, 0, PAR_NODE_COUNT);
    uint8_t fg_start = (uint8_t)((nc >> 16) & 0xFFu);
    uint8_t fg_count = (uint8_t)(nc & 0xFFu);

    for (uint8_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((get_param(cad, fg, PAR_FUNC_TYPE) & 0xFFu) != 0x01u) continue;

        uint32_t wc      = get_param(cad, fg, PAR_NODE_COUNT);
        uint8_t w_start  = (uint8_t)((wc >> 16) & 0xFFu);
        uint8_t w_count  = (uint8_t)(wc & 0xFFu);

        uint8_t dac = 0, pin = 0;
        for (uint8_t w = w_start; w < w_start + w_count; w++) {
            uint32_t cap   = get_param(cad, w, PAR_WIDGET_CAP);
            uint8_t  wtype = (uint8_t)((cap >> 20) & 0xFu);
            if (!dac && wtype == WTYPE_AUDIO_OUT) {
                dac = w;
            } else if (!pin && wtype == WTYPE_PIN) {
                if (get_param(cad, w, PAR_PIN_CAP) & (1u << 4))
                    pin = w;
            }
        }
        if (!dac || !pin) continue;

        verb_send(VERB12(cad, dac, V_SET_POWER, 0));
        verb_send(VERB12(cad, pin, V_SET_POWER, 0));
        verb_send(VERB4(cad, dac, V4_SET_FMT, FMT_48K_16BIT_STEREO));
        verb_send(VERB12(cad, dac, V_SET_STRM, (1u << 4) | 0u));  // tag=1, ch=0
        verb_send(VERB4(cad, dac, V4_SET_AMP, AMP_OUT_BOTH_UNMUTE));
        verb_send(VERB12(cad, pin, V_SET_CONN_SEL, 0));
        verb_send(VERB12(cad, pin, V_SET_PIN_CTL, PIN_OUT_EN | PIN_HP_EN));
        verb_send(VERB4(cad, pin, V4_SET_AMP, AMP_OUT_BOTH_UNMUTE));
        return 1;
    }
    return 0;
}

// ── Software FIFO helpers ─────────────────────────────────────────────────

static inline uint32_t fifo_used(void) {
    return (s_fifo_wp - s_fifo_rp) & FIFO_MASK;
}

static inline uint32_t fifo_free(void) {
    return FIFO_BYTES - 1u - fifo_used();
}

// Copy `want` bytes from FIFO into `dst`, zero-padding if FIFO is short.
// Called only from IRQ handler — no locking needed (single CPU).
static void fifo_drain(uint8_t* dst, uint32_t want) {
    uint32_t avail = fifo_used();
    uint32_t n     = (avail < want) ? avail : want;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = s_fifo[(s_fifo_rp + i) & FIFO_MASK];
    for (uint32_t i = n; i < want; i++)
        dst[i] = 0;  // silence remainder

    __asm__ volatile("mfence" ::: "memory");
    s_fifo_rp = (s_fifo_rp + n) & FIFO_MASK;
}

// ── IRQ handler ───────────────────────────────────────────────────────────
// Called once per DMA period (~21 ms).
// Refills the buffer hardware just finished, then wakes the producer.

void hda_irq_handler(void) {
    if (!s_ok) return;

    // Clear stream status (W1C) — must clear BCIS or IRQ stays asserted.
    uint8_t sts = r8(s_sd_base + SD_STS);
    w8(s_sd_base + SD_STS, sts);

    // Clear global interrupt status.
    uint32_t gis = r32(HDA_INTSTS);
    if (gis) w32(HDA_INTSTS, gis);

    // Only act on buffer completion (BCIS).
    if (!(sts & SD_STS_BCIS)) return;

    // Refill the DMA buffer that hardware just finished playing.
    // Hardware advances to (s_dma_wi + 1) when it fires IOC on s_dma_wi,
    // so s_dma_wi is the idle slot we can safely overwrite.
    fifo_drain(s_buf[s_dma_wi], BUF_BYTES);
    s_dma_wi = (uint8_t)((s_dma_wi + 1u) & (BDL_ENTRIES - 1u));

    // Wake producer if it is waiting for FIFO space.
    irq_notify(g_hda_irq);
}

// ── Public write API ──────────────────────────────────────────────────────
// Application calls this to push PCM.  Never touches DMA buffers.
// Blocks (IRQ-driven sleep) only when the FIFO is full.

int hda_write(const void* buf, uint32_t len) {
    if (!s_ok) return 0;

    const uint8_t* src  = (const uint8_t*)buf;
    uint32_t       done = 0;

    spin_lock(&s_hda_wlock);

    while (done < len) {
        uint32_t space = fifo_free();
        while (space == 0) {
            // Must not sleep holding the producer lock — drop it across the
            // IRQ wait, re-take after.  Another writer may make progress
            // meanwhile; we re-read space after re-acquiring.
            spin_unlock(&s_hda_wlock);
            irq_wait(g_hda_irq);
            spin_lock(&s_hda_wlock);
            space = fifo_free();
        }

        uint32_t copy = len - done;
        if (copy > space) copy = space;

        for (uint32_t i = 0; i < copy; i++)
            s_fifo[(s_fifo_wp + i) & FIFO_MASK] = src[done + i];
        __asm__ volatile("mfence" ::: "memory");
        s_fifo_wp = (s_fifo_wp + copy) & FIFO_MASK;
        done += copy;
    }

    // Start DMA engine lazily once we have enough data for the first period.
    // This avoids a cold-start silence gap.
    if (!s_running && fifo_used() >= BUF_BYTES) {
        s_running = 1;
        // Pre-fill the first BDL slot before starting so hardware has data
        // the instant it begins.
        fifo_drain(s_buf[0], BUF_BYTES);
        s_dma_wi = 1;  // next IRQ will fill slot 1
        sd_w32(SD_CTL, SD_CTL_TAG(1) | SD_CTL_IOCE | SD_CTL_FEIE | SD_CTL_DEIE | SD_CTL_RUN);
    }

    spin_unlock(&s_hda_wlock);
    return (int)done;
}

// ── Initialisation ────────────────────────────────────────────────────────

int hda_init(void) {
    // 1. Find Intel HDA PCI device (class 04h, subclass 03h).
    pci_device_t dev;
    if (!pci_find(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA, &dev)) return 0;

    // 2. Disable MSI/MSI-X so the PIC sees the legacy INTx line.
    //    Same pattern as ahci.c.
    {
        uint32_t status = pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x04u) >> 16;
        if (status & (1u << 4)) {
            uint8_t cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x34u) & 0xFCu);
            while (cap) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
                uint8_t  id = (uint8_t)(dw & 0xFFu);
                if (id == 0x05u)       // MSI
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap, dw & ~(1u << 16));
                else if (id == 0x11u)  // MSI-X
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap, dw & ~(1u << 31));
                cap = (uint8_t)((dw >> 8) & 0xFCu);
            }
        }
    }

    // 3. Enable MMIO decode + bus mastering.
    pci_enable(dev.bus, dev.dev, dev.fn);

    // 4. Map BAR0 (64-bit MMIO, up to 16 KiB).
    uint64_t bar = pci_bar_base(dev.bus, dev.dev, dev.fn, 0);
    s_mmio = (uint8_t*)vmm_map_mmio((phys_addr_t)bar, 0x4000u);
    if (!s_mmio) return 0;

    // 5. Controller reset: deassert (CRST=0) for ≥100 µs, then assert.
    w32(HDA_GCTL, 0);
    for (volatile int i = 0; i < 100000; i++);
    w32(HDA_GCTL, GCTL_CRST | GCTL_UNSOL);
    // Wait for codec to appear on the link (STATESTS non-zero).
    int cad = -1;
    for (int i = 0; i < 1000000; i++) {
        uint16_t st = r16(HDA_STATESTS);
        if (st) {
            for (int b = 0; b < 15; b++)
                if (st & (1u << b)) { cad = b; break; }
            break;
        }
    }
    if (cad < 0) return 0;
    w16(HDA_STATESTS, r16(HDA_STATESTS));   // clear wake bits

    // 6. CORB: 256 entries × 4 B = 1 KiB (fits in one 4 KiB page).
    phys_addr_t corb_phys = pmm_buddy_alloc(0);
    if (!corb_phys) return 0;
    s_corb = (uint32_t*)((uintptr_t)corb_phys + HHDM_OFFSET);
    __builtin_memset(s_corb, 0, 256 * sizeof(uint32_t));

    w8(HDA_CORBCTL, 0);
    w8(HDA_CORBSIZE, 0x02u);                               // 256 entries
    w32(HDA_CORBBASE,     (uint32_t)(corb_phys & 0xFFFFFFFFu));
    w32(HDA_CORBBASE + 4, (uint32_t)(corb_phys >> 32));
    w16(HDA_CORBRP, CORBRP_RST);
    for (volatile int i = 0; i < 10000; i++);
    w16(HDA_CORBRP, 0);
    w16(HDA_CORBWP, 0);
    w8(HDA_CORBCTL, CORBCTL_RUN);

    // 7. RIRB: 256 entries × 8 B = 2 KiB (fits in one page).
    phys_addr_t rirb_phys = pmm_buddy_alloc(0);
    if (!rirb_phys) return 0;
    s_rirb = (uint64_t*)((uintptr_t)rirb_phys + HHDM_OFFSET);
    __builtin_memset(s_rirb, 0, 256 * sizeof(uint64_t));

    w8(HDA_RIRBCTL, 0);
    w8(HDA_RIRBSIZE, 0x02u);                               // 256 entries
    w32(HDA_RIRBBASE,     (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    w32(HDA_RIRBBASE + 4, (uint32_t)(rirb_phys >> 32));
    w16(HDA_RIRBWP, (uint16_t)CORBRP_RST);                // reset WP
    for (volatile int i = 0; i < 10000; i++);
    w16(HDA_RINTCNT, 1u);
    s_rirb_rp = 0;
    w8(HDA_RIRBCTL, RIRBCTL_RUN | RIRBCTL_RINTFL);

    // 8. Determine which MMIO offset the first output stream starts at.
    //    GCAP[11:8] = ISS (input stream count).
    //    Output stream descriptors follow input stream descriptors.
    //    Each descriptor is 0x20 bytes.  Global registers end at 0x80.
    uint16_t gcap = r16(HDA_GCAP);
    s_iss = (uint8_t)((gcap >> 8) & 0xFu);
    s_sd_base = 0x80u + (uint32_t)s_iss * 0x20u;

    // 9. Reset the output stream: assert SRST, poll until confirmed, clear.
    sd_w32(SD_CTL, SD_CTL_SRST);
    for (volatile int i = 0; i < 50000; i++);
    for (int i = 0; i < 100000; i++) { if (sd_r32(SD_CTL) & SD_CTL_SRST) break; }
    sd_w32(SD_CTL, 0);
    for (volatile int i = 0; i < 10000; i++);
    for (int i = 0; i < 100000; i++) { if (!(sd_r32(SD_CTL) & SD_CTL_SRST)) break; }
    sd_w8(SD_STS, SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE);

    // 10. Allocate BDL (one page; 8 entries × 16 B = 128 B).
    phys_addr_t bdl_phys = pmm_buddy_alloc(0);
    if (!bdl_phys) return 0;
    s_bdl = (bdle_t*)((uintptr_t)bdl_phys + HHDM_OFFSET);

    // 11. Allocate DMA buffers (one per BDL entry) and populate BDL.
    //     All buffers start zeroed (silence).
    for (uint32_t i = 0; i < BDL_ENTRIES; i++) {
        phys_addr_t p = pmm_buddy_alloc(BUF_ORDER);
        if (!p) return 0;
        s_buf[i] = (uint8_t*)((uintptr_t)p + HHDM_OFFSET);
        for (uint32_t j = 0; j < BUF_BYTES; j++) s_buf[i][j] = 0;
        s_bdl[i].addr_lo = (uint32_t)(p & 0xFFFFFFFFu);
        s_bdl[i].addr_hi = (uint32_t)(p >> 32);
        s_bdl[i].length  = BUF_BYTES;
        s_bdl[i].flags   = 1u;  // IOC on every entry
    }

    // 12. Allocate software FIFO (one order-1 alloc = 2 pages = 8 KiB).
    phys_addr_t fifo_phys = pmm_buddy_alloc(1);
    if (!fifo_phys) return 0;
    s_fifo    = (uint8_t*)((uintptr_t)fifo_phys + HHDM_OFFSET);
    s_fifo_wp = 0;
    s_fifo_rp = 0;
    __builtin_memset(s_fifo, 0, FIFO_BYTES);

    // 13. Programme stream descriptor.
    sd_w32(SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    sd_w32(SD_BDPU, (uint32_t)(bdl_phys >> 32));
    sd_w32(SD_CBL,  BUF_BYTES * BDL_ENTRIES);
    sd_w16(SD_LVI,  (uint16_t)(BDL_ENTRIES - 1u));
    sd_w16(SD_FMT,  FMT_48K_16BIT_STEREO);
    // Tag=1, interrupts armed — but NOT RUN yet (started lazily by hda_write).
    sd_w32(SD_CTL,  SD_CTL_TAG(1) | SD_CTL_IOCE | SD_CTL_FEIE | SD_CTL_DEIE);

    // 14. Enable MSI and register the ISR.
    //     MSI bypasses the PIC and IOAPIC entirely.  The HDA controller writes
    //     a LAPIC-format message to memory when a stream completes.
    {
        uint8_t cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                               0x34u) & 0xFCu);
        while (cap) {
            uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
            if ((dw & 0xFFu) == 0x05u) {  // MSI capability
                uint32_t mc = dw;

                uint64_t msi_addr = lapic_msi_addr();
                pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                cap + 4u, (uint32_t)(msi_addr & 0xFFFFFFFFu));

                int is_64bit = (int)((mc >> 23) & 1u);
                if (is_64bit) {
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                    cap + 8u, (uint32_t)(msi_addr >> 32));
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                    cap + 12u, lapic_msi_data(VEC_HDA_MSI));
                } else {
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn,
                                    cap + 8u, lapic_msi_data(VEC_HDA_MSI));
                }

                // Enable MSI, single message.
                pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap,
                                (mc & ~(0x7u << 20)) | (1u << 16));
                break;
            }
            cap = (uint8_t)((dw >> 8) & 0xFCu);
        }

        g_hda_irq = HDA_IRQ_SLOT;
        idt_irq_register(VEC_HDA_MSI, (uint64_t)hda_irq_entry);
    }

    // 15. Enable global interrupt + stream interrupt for our output stream.
    //     Bit (s_iss + 0) enables output stream 0 interrupt.
    //     Do NOT set INTCTL_CIE (bit 30) — that is for RIRB overrun, not audio.
    w32(HDA_INTCTL, INTCTL_GIE | (1u << s_iss));

    // 16. Configure codec: enumerate widget graph, set up DAC→pin path.
    if (!codec_configure((uint8_t)cad)) return 0;

    // 17. Mark driver ready.  Stream starts lazily on first hda_write().
    s_dma_wi  = 0u;
    s_running = 0;
    s_ok      = 1;
    return 1;
}
