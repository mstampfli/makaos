#pragma once
#include "common.h"

// Initialise the NVMe driver: discover the controller via PCI, map BAR0,
// reset + enable, bring up the admin + first I/O queue, and identify the
// controller and first namespace.  Returns 1 on success, 0 if no NVMe
// controller was found or bring-up failed.
uint8_t nvme_init(void);

// Synchronous block I/O against the first namespace.  buf is a kernel
// HHDM or linear-map pointer.  nlb = number of LBAs (512 B each by
// default on QEMU NVMe).  Returns 1 on success, 0 on error.
// Polling-based; later steps will add MSI-X + async completion.
uint8_t nvme_read(uint64_t lba, void* buf, uint32_t nlb);
uint8_t nvme_write(uint64_t lba, const void* buf, uint32_t nlb);
