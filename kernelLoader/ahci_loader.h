#pragma once
#include "common.h"

/* Returns 1 on success, 0 on failure. */
uint8_t ahci_loader_init(void);

/* Read `count` sectors from `lba` into `buf`. */
uint8_t ahci_loader_read(uint64_t lba, void* buf, uint32_t count);
