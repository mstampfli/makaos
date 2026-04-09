#pragma once
#include "common.h"

// ── Intel AC97 Audio Controller driver ───────────────────────────────────
//
// Exposes a single write-only PCM stream at /dev/dsp.
//
// Format: signed 16-bit stereo, little-endian, interleaved L/R samples.
// Sample rate: 48000 Hz (AC97 hardware default; we advertise this to
// userspace so it can resample accordingly).
//
// The driver uses a double-buffered DMA ring of AC97 Buffer Descriptor
// List (BDL) entries.  Userspace writes fill one half while the DMA
// engine plays the other, giving continuous glitch-free output.

#define AC97_SAMPLE_RATE   48000   // Hz
#define AC97_CHANNELS      2       // stereo
#define AC97_BITS          16      // signed 16-bit

// Initialise the AC97 controller.  Finds it on the PCI bus, maps its
// I/O BARs, sets up DMA buffers and starts the PCM output engine.
// Returns 1 on success, 0 if no AC97 controller was found.
int ac97_init(void);

// Write PCM samples to the output stream.  `buf` must contain signed
// 16-bit stereo samples at AC97_SAMPLE_RATE Hz.  `len` is in bytes.
// Blocks (via IRQ-driven sleep) if the DMA ring is full.
// Returns the number of bytes actually written (always == len on success).
int ac97_write(const void* buf, uint32_t len);

// Called from the IRQ stub (irq_stubs.asm) when the AC97 signals buffer
// completion.  Notifies the sleeping writer via irq_notify().
void ac97_irq_handler(void);

// IRQ line assigned to this AC97 controller (read by irq_stubs.asm).
extern uint8_t g_ac97_irq;
