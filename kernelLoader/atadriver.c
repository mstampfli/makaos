#include "ata_driver.h"
#include "common.h"

static inline uint8_t ata_status_get(void) {
    return inb(ATA_REG_STATUS);
}

uint8_t ata_wait_idle(void) {
  uint32_t timeout = 100000000;
  while (timeout--) {
    uint8_t status = ata_status_get();
    if (status == 0xFF) return 0;
    if (status & ATA_SR_BSY) continue;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0; // Fail fast
    if (!(status & ATA_SR_DRDY)) continue;
    else return 1;
  }
  return 0;
}

uint8_t ata_wait_drq(void) {
  uint32_t timeout = 10000000;
  while (timeout--) {
    uint8_t status = ata_status_get();
    if (status == 0xFF) return 0;
    if (status & ATA_SR_BSY) continue;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0; // Fail fast
    if (status & ATA_SR_DRQ) return 1;
  }
  return 0;
}

uint8_t ata_init(void) {
  outb(ATA_REG_CONTROL, 0x02); // disable interrupts
  outb(ATA_REG_DRIVE_SEL, 0xE0); // master, LBA
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  uint32_t timeout = 10000000;
  while (timeout--) {
    uint8_t status = ata_status_get();
    if (status == 0xFF) return 0;

    if (status & ATA_SR_BSY) continue;
    if (status & ATA_SR_DF || status & ATA_SR_ERR) return 0;

    if (!(status & ATA_SR_DRDY)) continue;
    else break;
  }

  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  outb(ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  if (!ata_wait_idle()) return 0;

  uint8_t status = ata_status_get();
  if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
  if (status & ATA_SR_DRQ) return 0;

  return 1;
}

void ata_lba28_set(uint32_t lba) {
  outb(ATA_REG_LBA_LOW, (uint8_t)lba);
  outb(ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));

  outb(ATA_REG_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
}

uint8_t ata_disk_write_28_poll(uint32_t lba, const void* addr, uint32_t sector_count) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sector_count) {
    if (!ata_wait_idle()) return 0;

    ata_lba28_set(lba);

    // single-sector command path: issue 1 sector per command
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);   // 0x30
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!ata_wait_drq()) return 0;
    outsw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sector_count -= 1;
  }

  if (!ata_wait_idle()) return 0;
  return 1;
}

uint8_t ata_disk_read_28_poll(uint32_t lba, void* addr, uint32_t sector_count) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sector_count) {
    if (!ata_wait_idle()) return 0;

    ata_lba28_set(lba);

    // single-sector command path: issue 1 sector per command
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);    // 0x20
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!ata_wait_drq()) return 0;
    insw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sector_count -= 1;
  }

  if (!ata_wait_idle()) return 0;
  return 1;
}

void ata_lba48_set(uint64_t lba, uint32_t sector_count) {
  uint8_t count_high;
  uint8_t count_low;

  if (sector_count == 65536) {
    count_high = 0;
    count_low  = 0;
  } else {
    count_high = (uint8_t)(sector_count >> 8);
    count_low  = (uint8_t)(sector_count & 0xFF);
  }

  outb(ATA_REG_DRIVE_SEL, 0xE0);
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  outb(ATA_REG_SECCOUNT, count_high);

  outb(ATA_REG_LBA_LOW,  (uint8_t)(lba >> 24));
  outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 32));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));

  outb(ATA_REG_SECCOUNT, count_low);

  outb(ATA_REG_LBA_LOW,  (uint8_t)lba);
  outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 8));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));

  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
}

uint8_t ata_disk_write_48_poll(uint64_t lba, const void* addr, uint32_t sector_count) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sector_count) {
    if (!ata_wait_idle()) return 0;

    // single-sector command path: 1 sector per command
    ata_lba48_set(lba, 1);

    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS_EXT); // 0x34
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!ata_wait_drq()) return 0;
    outsw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sector_count -= 1;
  }

  if (!ata_wait_idle()) return 0;
  return 1;
}

uint8_t ata_disk_read_48_poll(uint64_t lba, void* addr, uint32_t sector_count) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sector_count) {
    if (!ata_wait_idle()) return 0;

    // single-sector command path: 1 sector per command
    ata_lba48_set(lba, 1);

    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS_EXT);  // 0x24
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!ata_wait_drq()) return 0;
    insw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sector_count -= 1;
  }

  if (!ata_wait_idle()) return 0;
  return 1;
}
