#pragma once
#include "common.h"

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

#define ATA_REG_DATA      0x1F0
#define ATA_REG_SECCOUNT  0x1F2
#define ATA_REG_LBA_LOW   0x1F3
#define ATA_REG_LBA_MID   0x1F4
#define ATA_REG_LBA_HIGH  0x1F5
#define ATA_REG_DRIVE_SEL 0x1F6
#define ATA_REG_COMMAND   0x1F7
#define ATA_REG_STATUS    0x1F7
#define ATA_REG_CONTROL   0x3F6

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30

// Wait for BSY=0, ERR=0, DRDY=1, DRQ=0.  Returns 1 on success, 0 on timeout/error.
uint8_t ata_poll_wait_idle(void);

// Wait for BSY=0, DRQ=1.  Returns 1 on success, 0 on timeout/error.
uint8_t ata_poll_wait_drq(void);

// Initialize ATA primary master (disables interrupts, selects drive).
// Returns 1 on success.
uint8_t ata_poll_init(void);

// Read `count` sectors starting at LBA28 `lba` into `buf`.
// Returns 1 on success, 0 on error.
uint8_t ata_poll_read28(uint32_t lba, void* buf, uint32_t count);

// Write `count` sectors starting at LBA28 `lba` from `buf`.
// Returns 1 on success, 0 on error.
uint8_t ata_poll_write28(uint32_t lba, const void* buf, uint32_t count);
