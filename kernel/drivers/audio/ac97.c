#include "ac97.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "irq_wait.h"
#include "idt.h"
#include "pic.h"
#include "common.h"

extern void ac97_irq_entry(void);   // defined in irq_stubs.asm

// ── PCI identifiers ───────────────────────────────────────────────────────
#define PCI_CLASS_MULTIMEDIA  0x04
#define PCI_SUBCLASS_AUDIO    0x01

// ── AC97 NAM (Native Audio Mixer) register offsets ────────────────────────
// These are I/O port offsets from BAR0.
#define NAM_RESET           0x00   // write anything to soft-reset codec
#define NAM_MASTER_VOL      0x02   // master output volume
#define NAM_HEADPHONE_VOL   0x04   // headphone volume
#define NAM_MONO_VOL        0x06   // mono output volume
#define NAM_PCM_VOL         0x18   // PCM output volume
#define NAM_SAMPLE_RATE     0x2C   // PCM front DAC sample rate

// Volume register encoding:
//   bits[14:8] = left attenuation (0 = 0 dB, 0x3F = mute)
//   bits[6:0]  = right attenuation
//   bit[15]    = mute
#define VOL_UNMUTE_0DB   0x0000   // left=0, right=0, unmuted

// ── AC97 NABM (Native Audio Bus Master) register offsets ─────────────────
// These are I/O port offsets from BAR1.
// Each channel has a 16-byte register block.  PCM Out is at base 0x10.
#define NABM_PCM_OUT_BASE   0x10

// Per-channel register offsets (relative to channel base):
#define NABM_BDBAR   0x00   // Buffer Descriptor Base Address Register (32-bit)
#define NABM_CIV     0x04   // Current Index Value (8-bit, read-only)
#define NABM_LVI     0x05   // Last Valid Index (8-bit, writable)
#define NABM_SR      0x06   // Status Register (16-bit)
#define NABM_PICB    0x08   // Position In Current Buffer (16-bit)
#define NABM_PIV     0x0A   // Prefetched Index Value (8-bit)
#define NABM_CR      0x0B   // Control Register (8-bit)

// NABM CR bits
#define CR_RUN       (1 << 0)   // DMA engine run
#define CR_RESET     (1 << 1)   // channel reset
#define CR_LVBIE     (1 << 2)   // last-valid-buffer interrupt enable
#define CR_FEIE      (1 << 3)   // FIFO error interrupt enable
#define CR_IOCE      (1 << 4)   // IOC interrupt enable

// NABM SR bits
#define SR_DCH       (1 << 0)   // DMA controller halted
#define SR_CELV      (1 << 1)   // current equals last valid
#define SR_LVBCI     (1 << 2)   // last valid buffer completion interrupt
#define SR_BCIS      (1 << 3)   // buffer completion interrupt status
#define SR_FIFOE     (1 << 4)   // FIFO error

// NABM global registers
#define NABM_GLOB_CNT   0x2C   // Global Control (32-bit)
#define NABM_GLOB_STS   0x30   // Global Status  (32-bit)

// GLOB_CNT bits
#define GLOB_CNT_GIE    (1 << 0)   // GPI interrupt enable
#define GLOB_CNT_COLD   (1 << 1)   // cold reset (1 = normal operation)

// ── Buffer Descriptor List ────────────────────────────────────────────────
// The AC97 BDL is an array of up to 32 entries.  Each entry describes one
// DMA buffer (physically contiguous, up to 65536 samples = 128 KiB stereo16).

#define BDL_ENTRIES     32        // AC97 hardware maximum
#define BDL_IOC         (1 << 31) // flag: interrupt on completion
#define BDL_BUP         (1 << 30) // flag: buffer underrun policy (stop on last)

// Each DMA buffer: 16384 bytes = 4096 stereo 16-bit samples ≈ 85 ms @ 48 kHz.
// order-2 buddy allocation = 4 contiguous physical pages per buffer.
// Larger buffers give the writer more headroom between IRQs and eliminate
// audible glitches at buffer boundaries under scheduler jitter.
#define DMA_BUF_ORDER   2                             // 2^2 = 4 pages per buffer
#define DMA_BUF_BYTES   (4096u << DMA_BUF_ORDER)     // 16384 bytes
#define DMA_BUF_SAMPLES (DMA_BUF_BYTES / (AC97_CHANNELS * (AC97_BITS / 8)))

typedef struct {
    uint32_t addr;   // physical address of PCM data (must be 32-bit)
    uint32_t ctl;    // bits[15:0] = number of samples, bits[31:30] = flags
} __attribute__((packed)) bdl_entry_t;

// ── Driver state ──────────────────────────────────────────────────────────

static int      s_present   = 0;      // 1 if AC97 was found and initialised
static uint16_t s_nam_base  = 0;      // NAM I/O base (BAR0)
static uint16_t s_nabm_base = 0;      // NABM I/O base (BAR1)

// IRQ line — written by ac97_init(), read by irq_stubs.asm via extern.
uint8_t g_ac97_irq = 0xFF;

// BDL and DMA buffers live in physically-contiguous kernel memory.
// We allocate one page for the BDL and one page per DMA buffer.
static bdl_entry_t* s_bdl = NULL;          // virtual address of BDL
static uint32_t     s_bdl_phys = 0;        // physical address of BDL
static uint8_t*     s_dma_bufs[BDL_ENTRIES]; // virtual addresses of DMA bufs
static uint32_t     s_dma_phys[BDL_ENTRIES]; // physical addresses of DMA bufs

// Write pointer into the DMA ring: which BDL slot we will fill next.
// The hardware's CIV tells us which slot it is currently consuming.
static uint8_t  s_write_idx  = 0;   // next slot for the producer to fill
static uint32_t s_write_off  = 0;   // byte offset within the current slot
static uint8_t  s_slots_used = 0;   // software-tracked filled slot count (0..BDL_ENTRIES)
static uint8_t  s_last_civ   = 0;   // CIV value at last ring_sync() call


// ── NAM / NABM I/O helpers ────────────────────────────────────────────────

static uint16_t nam_read16(uint8_t reg)  { return inw((uint16_t)(s_nam_base  + reg)); }
static void     nam_write16(uint8_t reg, uint16_t v) { outw((uint16_t)(s_nam_base  + reg), v); }

static uint8_t  nabm_read8 (uint8_t reg) { return inb((uint16_t)(s_nabm_base + reg)); }
static uint16_t nabm_read16(uint8_t reg) { return inw((uint16_t)(s_nabm_base + reg)); }
static uint32_t nabm_read32(uint8_t reg) { return inl((uint16_t)(s_nabm_base + reg)); }
static void     nabm_write8 (uint8_t reg, uint8_t  v) { outb((uint16_t)(s_nabm_base + reg), v); }
static void     nabm_write16(uint8_t reg, uint16_t v) { outw((uint16_t)(s_nabm_base + reg), v); }
static void     nabm_write32(uint8_t reg, uint32_t v) { outl((uint16_t)(s_nabm_base + reg), v); }

// Convenience: per-channel register access.
static inline uint8_t  pcm_read8 (uint8_t r) { return nabm_read8 ((uint8_t)(NABM_PCM_OUT_BASE + r)); }
static inline uint16_t pcm_read16(uint8_t r) { return nabm_read16((uint8_t)(NABM_PCM_OUT_BASE + r)); }
static inline void     pcm_write8 (uint8_t r, uint8_t  v) { nabm_write8 ((uint8_t)(NABM_PCM_OUT_BASE + r), v); }
static inline void     pcm_write16(uint8_t r, uint16_t v) { nabm_write16((uint8_t)(NABM_PCM_OUT_BASE + r), v); }
static inline void     pcm_write32(uint8_t r, uint32_t v) { nabm_write32((uint8_t)(NABM_PCM_OUT_BASE + r), v); }

// ── Ring management ───────────────────────────────────────────────────────

// Sync the software slot counter against hardware CIV.
// CIV is the slot the hardware is *currently playing*.  When CIV advances
// past s_last_civ, those slots have been consumed and are now free.
// We compute the delta as (civ - s_last_civ) mod 32, which is unambiguous
// as long as the hardware never skips more than 31 slots between syncs
// (guaranteed since we wake on every IOC).
static void ring_sync(void) {
    if (s_slots_used == 0) return;

    uint8_t civ   = pcm_read8(NABM_CIV);
    uint8_t delta = (uint8_t)((civ - s_last_civ) & (BDL_ENTRIES - 1));

    if (delta > s_slots_used) delta = s_slots_used;  // clamp: never go negative
    s_slots_used -= delta;
    s_last_civ    = civ;
}

// Advance LVI to tell hardware about newly filled slots.
static void ring_commit(void) {
    // LVI = last valid index = write_idx - 1 (the most recently filled slot).
    uint8_t lvi = (uint8_t)((s_write_idx - 1) & (BDL_ENTRIES - 1));
    pcm_write8(NABM_LVI, lvi);
}

// Start the PCM output DMA engine if it's not already running.
// Always preserves CR_IOCE so interrupts keep firing after start.
static void pcm_start(void) {
    uint8_t cr = pcm_read8(NABM_CR);
    if (!(cr & CR_RUN))
        pcm_write8(NABM_CR, (uint8_t)(cr | CR_RUN | CR_IOCE));
}

// ── IRQ handler (called from ac97_irq_entry in irq_stubs.asm) ────────────

void ac97_irq_handler(void) {
    // Acknowledge all pending status bits on the PCM Out channel.
    uint16_t sr = pcm_read16(NABM_SR);
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE))
        pcm_write16(NABM_SR, (uint16_t)(sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)));

    // Wake any task waiting for a free buffer slot.
    if (g_ac97_irq != 0xFF)
        irq_notify(g_ac97_irq);
}

// ── Internal: commit the current partial/full DMA slot to hardware ────────
// Pads any remaining bytes with silence (signed 16-bit zero = 0x00).
// Must only be called when s_write_off > 0 (slot has data in it).
static void slot_commit(void) {
    uint8_t slot = s_write_idx;

    // Pad remainder of slot with silence so hardware plays valid data.
    uint32_t pad = DMA_BUF_BYTES - s_write_off;
    uint8_t* dst = s_dma_bufs[slot] + s_write_off;
    for (uint32_t i = 0; i < pad; i++) dst[i] = 0;

    // Number of samples actually in this slot (including the silent pad).
    // Hardware will play the full slot; silence padding is inaudible.
    s_bdl[slot].ctl = (uint32_t)DMA_BUF_SAMPLES | BDL_IOC;
    s_write_idx     = (uint8_t)((slot + 1) & (BDL_ENTRIES - 1));
    s_write_off     = 0;
    if (s_slots_used < BDL_ENTRIES) s_slots_used++;
    ring_commit();
    pcm_start();
}

// ── Public API: ac97_write ────────────────────────────────────────────────
//
// Write `len` bytes of signed 16-bit stereo PCM at 48 kHz to the DMA ring.
// Blocks (IRQ-driven sleep) when the ring is full.
// At the end of each call, any partial DMA slot is padded with silence and
// committed immediately so hardware never stalls between calls.

int ac97_write(const void* buf, uint32_t len) {
    if (!s_present) return 0;

    const uint8_t* src   = (const uint8_t*)buf;
    uint32_t       total = 0;

    while (total < len) {
        // Wait until at least one slot is free.
        // Keep a 2-slot margin: LVI must never equal CIV or DMA halts.
        ring_sync();
        while (s_slots_used >= (uint8_t)(BDL_ENTRIES - 2)) {
            irq_wait(g_ac97_irq);
            ring_sync();
        }

        uint8_t  slot = s_write_idx;
        uint8_t* dst  = s_dma_bufs[slot] + s_write_off;
        uint32_t room = DMA_BUF_BYTES - s_write_off;
        uint32_t copy = len - total;
        if (copy > room) copy = room;

        for (uint32_t i = 0; i < copy; i++) dst[i] = src[total + i];
        total       += copy;
        s_write_off += copy;

        if (s_write_off >= DMA_BUF_BYTES) {
            // Slot is exactly full — commit with no padding needed.
            s_bdl[slot].ctl = (uint32_t)DMA_BUF_SAMPLES | BDL_IOC;
            s_write_idx     = (uint8_t)((slot + 1) & (BDL_ENTRIES - 1));
            s_write_off     = 0;
            if (s_slots_used < BDL_ENTRIES) s_slots_used++;
            ring_commit();
            pcm_start();
        }
    }

    // Commit any partial slot left over at the end of this write call.
    // This prevents hardware from stalling between write() calls (e.g. one
    // Doom tic = 5488 bytes = 1 full slot + 1392 bytes partial).
    if (s_write_off > 0) {
        ring_sync();
        while (s_slots_used >= (uint8_t)(BDL_ENTRIES - 2)) {
            irq_wait(g_ac97_irq);
            ring_sync();
        }
        slot_commit();
    }

    return (int)total;
}

// ── Initialisation ────────────────────────────────────────────────────────

int ac97_init(void) {
    // 1. Locate AC97 audio controller on PCI bus.
    pci_device_t dev;
    if (!pci_find(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, &dev)) return 0;

    // 2. Enable MMIO + bus mastering, read I/O BARs.
    pci_enable(dev.bus, dev.dev, dev.fn);

    uint64_t bar0 = pci_bar_base(dev.bus, dev.dev, dev.fn, 0);
    uint64_t bar1 = pci_bar_base(dev.bus, dev.dev, dev.fn, 1);

    // AC97 BARs are I/O space (bit 0 set in raw BAR).  pci_bar_base already
    // masks out the type bits, so bar0/bar1 are the I/O base addresses.
    s_nam_base  = (uint16_t)bar0;
    s_nabm_base = (uint16_t)bar1;

    // 3. Cold reset: assert then deassert global cold reset.
    nabm_write32(NABM_GLOB_CNT, 0x00000002);  // cold reset assert
    // brief spin (a few microseconds is enough for real HW; in QEMU instant)
    for (volatile int i = 0; i < 10000; i++);
    nabm_write32(NABM_GLOB_CNT, GLOB_CNT_COLD);

    // 4. Soft-reset the codec via NAM.
    nam_write16(NAM_RESET, 0xFFFF);
    for (volatile int i = 0; i < 10000; i++);

    // 5. Set output volumes (unmute, 0 dB attenuation).
    nam_write16(NAM_MASTER_VOL,    VOL_UNMUTE_0DB);
    nam_write16(NAM_HEADPHONE_VOL, VOL_UNMUTE_0DB);
    nam_write16(NAM_MONO_VOL,      VOL_UNMUTE_0DB);
    nam_write16(NAM_PCM_VOL,       VOL_UNMUTE_0DB);

    // 6. Set sample rate (if VRA is supported; QEMU's AC97 supports it).
    //    Some codecs ignore this and lock at 48 kHz; that's fine — we always
    //    output at AC97_SAMPLE_RATE anyway.
    nam_write16(NAM_SAMPLE_RATE, AC97_SAMPLE_RATE);
    // Read back to confirm (not fatal if it doesn't stick).
    (void)nam_read16(NAM_SAMPLE_RATE);

    // 7. Reset PCM Out channel.
    pcm_write8(NABM_CR, CR_RESET);
    for (volatile int i = 0; i < 10000; i++);
    pcm_write8(NABM_CR, 0);
    // Clear any stale status bits.
    pcm_write16(NABM_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);

    // 8. Allocate BDL (one page, must be below 4 GiB for AC97 32-bit DMA).
    // Check the FULL phys_addr_t for the OOM sentinel BEFORE truncating to
    // uint32: pmm_buddy_alloc returns PMM_INVALID_ADDR (UINT64_MAX), and the
    // old (uint32_t) cast turned that into 0xFFFFFFFF -- which `!s_bdl_phys`
    // never caught, so OOM was treated as a (wild, mapped) 4 GiB address.
    phys_addr_t bdl_alloc = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(bdl_alloc)) return 0;
    s_bdl_phys = (uint32_t)bdl_alloc;
    s_bdl = (bdl_entry_t*)(uint64_t)(s_bdl_phys + HHDM_OFFSET);

    // 9. Allocate DMA buffers (4 contiguous pages each for 85 ms headroom).
    for (int i = 0; i < BDL_ENTRIES; i++) {
        phys_addr_t p = pmm_buddy_alloc(DMA_BUF_ORDER);
        if (!PMM_ALLOC_OK(p)) return 0;
        s_dma_phys[i] = (uint32_t)p;
        s_dma_bufs[i] = (uint8_t*)(p + HHDM_OFFSET);
        // Silence the buffer initially.
        for (uint32_t j = 0; j < DMA_BUF_BYTES; j++) s_dma_bufs[i][j] = 0;
        // Wire into BDL: address + count (samples) + no flags yet.
        s_bdl[i].addr = s_dma_phys[i];
        s_bdl[i].ctl  = (uint32_t)DMA_BUF_SAMPLES;
    }

    // 10. Point PCM Out channel at the BDL.
    pcm_write32(NABM_BDBAR, s_bdl_phys);

    // 11. Ring starts empty — DMA not yet running.
    //     ac97_write() will commit the first slot and start the engine.
    s_write_idx  = 0;
    s_write_off  = 0;
    s_slots_used = 0;
    s_last_civ   = 0;

    // 12. Register IRQ — same PIIX3 PIRQ routing as AHCI.
    //     i440fx formula: pirq_idx = (slot + intpin - 2) % 4
    //     We route the AC97's PIRQ to a dedicated IRQ line (5) to avoid
    //     sharing with AHCI (IRQ 11).
    {
        #define AC97_IRQ    5u
        #define PIIX3_BUS   0
        #define PIIX3_DEV   1
        #define PIIX3_FN    0

        uint32_t cfg3c  = pci_cfg_read32(dev.bus, dev.dev, dev.fn, 0x3C);
        uint8_t  intpin = (uint8_t)((cfg3c >> 8) & 0xFF);
        if (intpin == 0 || intpin > 4) intpin = 1;

        uint8_t pirq_idx = (uint8_t)((dev.dev + intpin - 2u) % 4u);

        // Program the PIRQ routing register in the PIIX3 bridge.
        uint32_t pirq_dw = pci_cfg_read32(PIIX3_BUS, PIIX3_DEV, PIIX3_FN, 0x60);
        uint32_t shift   = pirq_idx * 8u;
        pirq_dw = (pirq_dw & ~(0xFFu << shift)) | ((uint32_t)AC97_IRQ << shift);
        pci_cfg_write32(PIIX3_BUS, PIIX3_DEV, PIIX3_FN, 0x60, pirq_dw);

        // Update the device Interrupt Line register.
        pci_cfg_write32(dev.bus, dev.dev, dev.fn, 0x3C,
                        (cfg3c & ~0xFFu) | AC97_IRQ);

        g_ac97_irq = AC97_IRQ;
        uint8_t vec = (AC97_IRQ < 8u) ? (uint8_t)(0x20u + AC97_IRQ)
                                       : (uint8_t)(0x28u + AC97_IRQ - 8u);
        idt_irq_register(vec, (uint64_t)ac97_irq_entry);
        pic_unmask(AC97_IRQ);
    }

    // 13. Enable buffer-completion interrupts on PCM Out channel.
    pcm_write8(NABM_CR, CR_IOCE);

    s_present = 1;
    return 1;
}
