#pragma once
#include "common.h"

// Initialize AHCI: locate the controller via PCI, map its registers,
// and bring up the first available SATA port.
// Returns 1 on success, 0 if no AHCI controller/disk found.
uint8_t ahci_init(void);

// Read `count` 512-byte sectors starting at 48-bit LBA `lba` into `buf`.
// Returns 1 on success, 0 on error.
uint8_t ahci_read(uint64_t lba, void* buf, uint32_t count);

// Write `count` 512-byte sectors from `buf` to 48-bit LBA `lba`.
// Returns 1 on success, 0 on error.
uint8_t ahci_write(uint64_t lba, const void* buf, uint32_t count);
