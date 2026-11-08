#pragma once
#include "common.h"

// ── Intel High Definition Audio (HDA/Azalia) driver ─────────────────────
//
// Exposes a write-only PCM output stream at /dev/dsp.
//
// Format: signed 16-bit stereo, little-endian, interleaved L/R.
// Sample rate: 48000 Hz.
//
// The driver enumerates the codec widget graph, configures the first DAC →
// pin output path, and streams PCM via a cyclic BDL-backed DMA engine.
// The writer blocks (IRQ-driven sleep) when the ring is full.

#define HDA_SAMPLE_RATE   48000
#define HDA_CHANNELS      2
#define HDA_BITS          16

// Initialise the HDA controller.  Returns 1 on success, 0 if not found.
int hda_init(void);

// Write `len` bytes of signed 16-bit stereo PCM at 48 kHz.
// Blocks when the DMA ring is full.  Returns bytes written (== len).
int hda_write(const void* buf, uint32_t len);

// Poll predicate for /dev/dsp POLLOUT: returns nonzero when the software
// FIFO has room for at least a typical playback buffer (>= half the FIFO),
// so a subsequent hda_write() of that size lands without blocking.  Backs
// f->poll on /dev/dsp; the matching wake source is irq_waitq(g_hda_irq),
// the queue the DMA-completion ISR drains as it frees FIFO space.
int hda_poll_writable(void);

// IRQ handler — called from irq_stubs.asm.
void hda_irq_handler(void);

// IRQ line set by hda_init(), read by irq_stubs.asm.
extern uint8_t g_hda_irq;
