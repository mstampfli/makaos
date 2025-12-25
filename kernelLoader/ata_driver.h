#pragma once
#include "common.h"
#include "idt.h"

// bit 7: busy; ignore other bits if set
#define ATA_SR_BSY  0x80  
// bit 6: device ready
#define ATA_SR_DRDY 0x40  
// bit 5: device fault / write fault
#define ATA_SR_DF   0x20  
#define ATA_SR_DWF  0x20  
// bit 3: data request; ready for pio
#define ATA_SR_DRQ  0x08  
// bit 0: error; check error register
#define ATA_SR_ERR  0x01  

// primary bus ports
#define ATA_REG_DATA       0x1F0
#define ATA_REG_FEATURES   0x1F1
#define ATA_REG_SECCOUNT   0x1F2
#define ATA_REG_LBA_LOW    0x1F3
#define ATA_REG_LBA_MID    0x1F4
#define ATA_REG_LBA_HIGH   0x1F5
#define ATA_REG_DRIVE_SEL  0x1F6 // bits 0-3: lba bits 24-27; bit 4: drive; bit 6: lba mode
#define ATA_REG_COMMAND    0x1F7
#define ATA_REG_STATUS     0x1F7
#define ATA_REG_CONTROL    0x3F6

// ATA control (Device Control / Alternate Status port base, primary=0x3F6)
#define ATA_CTRL_SRST 0x04  // Software reset
#define ATA_CTRL_nIEN 0x02  // Interrupt enable: 0=enable IRQ, 1=disable IRQ
#define ATA_CTRL_IRQ_ENABLE 0x00                          
#define ATA_CTRL_IRQ_DISABLE 0x02                           

#define ATA_CMD_READ_MULTIPLE       0xC4
#define ATA_CMD_WRITE_MULTIPLE      0xC5

#define ATA_CMD_READ_MULTIPLE_EXT   0x29
#define ATA_CMD_WRITE_MULTIPLE_EXT  0x39
#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30

/* LBA48 (EXT) */
#define ATA_CMD_READ_SECTORS_EXT    0x24
#define ATA_CMD_WRITE_SECTORS_EXT   0x34
#define ATA_CMD_SET_MULTIPLE        0xC6

// initializes ata controller and sets multiple mode
uint8_t ata_init(void);

// reads sectors from disk using lba28
uint8_t ata_disk_read_28_poll(uint32_t lba, void* addr, uint32_t sector_count);

// writes sectors to disk using lba28
uint8_t ata_disk_write_28_poll(uint32_t lba, const void* addr, uint32_t sector_count);

// reads sectors from disk using lba48
uint8_t ata_disk_read_48_poll(uint64_t lba, void* addr, uint32_t sector_count);


// writes sectors to disk using lba48
uint8_t ata_disk_write_48_poll(uint64_t lba, const void* addr, uint32_t sector_count);

// waits for bsy to clear
uint8_t ata_wait_idle(void);

// waits for bsy to clear and drq to set
uint8_t ata_wait_drq(void);
