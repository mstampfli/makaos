#include "ata_driver.h"
#include "common.h"

static inline uint8_t getStatusATA(void) {
    return inb(ATA_REG_STATUS);
}

uint8_t waitForIdleATA(void) {
  uint32_t timeout = 100000000;
  while (timeout--) {
    uint8_t status = getStatusATA();
    if (status == 0xFF) return 0;
    if (status & ATA_SR_BSY) continue;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0; // Fail fast
    if (!(status & ATA_SR_DRDY)) continue;
    else return 1;
  }
  return 0;
}

uint8_t waitForDRQATA(void) {
  uint32_t timeout = 10000000;
  while (timeout--) {
    uint8_t status = getStatusATA();
    if (status == 0xFF) return 0;
    if (status & ATA_SR_BSY) continue;
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0; // Fail fast
    if (status & ATA_SR_DRQ) return 1;
  }
  return 0;
}

uint8_t initializeATA(void) {
  outb(ATA_REG_CONTROL, 0x02); // disable interrupts
  outb(ATA_REG_DRIVE_SEL, 0xE0); // master, LBA
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  uint32_t timeout = 10000000;
  while (timeout--) {
    uint8_t status = getStatusATA();
    if (status == 0xFF) return 0;

    if (status & ATA_SR_BSY) continue;
    if (status & ATA_SR_DF || status & ATA_SR_ERR) return 0;

    if (!(status & ATA_SR_DRDY)) continue;
    else break;
  }

  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  // unchanged (you can leave it; it won't be used by single-sector commands)
  outb(ATA_REG_SECCOUNT, 16);
  outb(ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  if (!waitForIdleATA()) return 0;

  uint8_t st = getStatusATA();
  if (st & (ATA_SR_ERR | ATA_SR_DF)) return 0;
  if (st & ATA_SR_DRQ) return 0;

  return 1;
}

void setupLBADest28(uint32_t lba) {
  outb(ATA_REG_LBA_LOW, (uint8_t)lba);
  outb(ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));

  outb(ATA_REG_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
}

uint8_t writeToDiskATA28(uint32_t lba, const void* addr, uint32_t sectorCount) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sectorCount) {
    if (!waitForIdleATA()) return 0;

    setupLBADest28(lba);

    // single-sector command path: issue 1 sector per command
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);   // 0x30
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!waitForDRQATA()) return 0;
    outsw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sectorCount -= 1;
  }

  if (!waitForIdleATA()) return 0;
  return 1;
}

uint8_t readFromDiskATA28(uint32_t lba, void* addr, uint32_t sectorCount) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sectorCount) {
    if (!waitForIdleATA()) return 0;

    setupLBADest28(lba);

    // single-sector command path: issue 1 sector per command
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);    // 0x20
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!waitForDRQATA()) return 0;
    insw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sectorCount -= 1;
  }

  if (!waitForIdleATA()) return 0;
  return 1;
}

void setupLBADest48(uint64_t lba, uint32_t sectorCount) {
  uint8_t countHigh;
  uint8_t countLow;

  if (sectorCount == 65536) {
    countHigh = 0;
    countLow  = 0;
  } else {
    countHigh = (uint8_t)(sectorCount >> 8);
    countLow  = (uint8_t)(sectorCount & 0xFF);
  }

  outb(ATA_REG_DRIVE_SEL, 0xE0);
  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

  outb(ATA_REG_SECCOUNT, countHigh);

  outb(ATA_REG_LBA_LOW,  (uint8_t)(lba >> 24));
  outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 32));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));

  outb(ATA_REG_SECCOUNT, countLow);

  outb(ATA_REG_LBA_LOW,  (uint8_t)lba);
  outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 8));
  outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));

  for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
}

uint8_t writeToDiskATA48(uint64_t lba, const void* addr, uint32_t sectorCount) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sectorCount) {
    if (!waitForIdleATA()) return 0;

    // single-sector command path: 1 sector per command
    setupLBADest48(lba, 1);

    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS_EXT); // 0x34
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!waitForDRQATA()) return 0;
    outsw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sectorCount -= 1;
  }

  if (!waitForIdleATA()) return 0;
  return 1;
}

uint8_t readFromDiskATA48(uint64_t lba, void* addr, uint32_t sectorCount) {
  uint16_t* ptr = (uint16_t*)addr;

  while (sectorCount) {
    if (!waitForIdleATA()) return 0;

    // single-sector command path: 1 sector per command
    setupLBADest48(lba, 1);

    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS_EXT);  // 0x24
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    if (!waitForDRQATA()) return 0;
    insw(ATA_REG_DATA, ptr, 256);

    ptr += 256;
    lba += 1;
    sectorCount -= 1;
  }

  if (!waitForIdleATA()) return 0;
  return 1;
}
