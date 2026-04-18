#pragma once
#include "common.h"
#include "pmm.h"

// Maximum DMA transfer in 512-byte sectors per single I/O command.
// Limited by the bounce buffer size in polling mode (BOUNCE_ORDER pages).
// Must stay in sync with BOUNCE_ORDER / BOUNCE_SECTORS in ahci.c.
#define AHCI_DMA_SECTORS 1024u   // 512 KiB

// Initialise AHCI: locate the controller via PCI, map its registers, and
// bring up the first available SATA port.  Sets up MSI-X (preferred) or
// MSI for lowest-priority delivery — IRQs route to whichever CPU has the
// lowest current task priority without any per-request steering.
// Returns 1 on success, 0 if no AHCI controller/disk found.
uint8_t ahci_init(void);

// Arm the NCQ/IRQ path.  Must be called after sched_init() so that
// per-slot wait queues can call sched_sleep().  No kthread is created;
// any CPU that calls ahci_read/ahci_write submits its own NCQ command
// and sleeps on a per-slot wait queue until the IRQ handler wakes it.
void ahci_start_io_thread(void);

// Poll for completed commands that the ISR may have missed (lost MSI-X).
// Called from sched_tick on every timer interrupt as a fallback — adds
// negligible overhead (a few MMIO reads) but guarantees no command hangs
// longer than one tick (~1ms) due to a lost interrupt.
void ahci_poll_completions(void);

// Read `count` 512-byte sectors starting at 48-bit LBA `lba` into `buf`.
// `buf` must be a kernel HHDM pointer (va = phys + HHDM_OFFSET).
// Returns 1 on success, 0 on error.
uint8_t ahci_read(uint64_t lba, void* buf, uint32_t count);

// Write `count` sectors from `buf` to LBA `lba`.
// Returns 1 on success, 0 on error.
uint8_t ahci_write(uint64_t lba, const void* buf, uint32_t count);

// Read `count` sectors at `lba` into a user-space buffer.
// Resolves user pages to physical frames via page tables and builds a
// scatter-gather PRDT so DMA goes straight to user memory — zero copies.
// Returns 1 on success, 0 on error.
uint8_t ahci_read_user(uint64_t lba, void* user_buf, uint32_t count);

// Parallel multi-page read for read-ahead prefetch.
//
// Submits up to `nframes` independent NCQ read commands simultaneously
// (all in-flight at once) then waits for each to complete.  Because all
// reads are in the HBA's NCQ queue before any wait, the HBA can reorder
// them for optimal rotational latency — approaching disk bandwidth for
// sequential file access.
//
// frames[i]: physical address of a pre-allocated PMM frame (from
//            pmm_buddy_alloc(0)), or PMM_INVALID_ADDR to skip index i.
//            Each read transfers exactly PAGE_SIZE bytes (8 sectors).
// lba:       LBA of frames[0]; frames[i] reads from lba + i * (PAGE_SIZE/512).
// nframes:   1..32 (capped at 32 internally).
//
// Returns a bitmask: bit i = 1 if frames[i] was successfully filled.
// Caller owns all frames regardless of outcome.
uint32_t ahci_read_multi(uint64_t lba, phys_addr_t* frames, uint32_t nframes);
