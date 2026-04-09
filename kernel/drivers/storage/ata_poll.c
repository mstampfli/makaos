#include "ata_poll.h"
#include "common.h"

uint8_t ata_poll_wait_idle(void) {
    uint32_t timeout = 100000000;
    while (timeout--) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st == 0xFF) return 0;
        if (st & ATA_SR_BSY)              continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if (st & ATA_SR_DRQ)              continue;
        if (!(st & ATA_SR_DRDY))          continue;
        return 1;
    }
    return 0;
}

uint8_t ata_poll_wait_drq(void) {
    uint32_t timeout = 10000000;
    while (timeout--) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st == 0xFF) return 0;
        if (st & ATA_SR_BSY)              continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if (st & ATA_SR_DRQ)              return 1;
    }
    return 0;
}

uint8_t ata_poll_init(void) {
    outb(ATA_REG_CONTROL, 0x02);   // disable interrupts
    outb(ATA_REG_DRIVE_SEL, 0xE0); // master, LBA
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

    uint32_t timeout = 10000000;
    while (timeout--) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st == 0xFF) return 0;
        if (st & ATA_SR_BSY)              continue;
        if (st & (ATA_SR_DF | ATA_SR_ERR)) return 0;
        if (!(st & ATA_SR_DRDY))          continue;
        break;
    }
    for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
    return 1;
}

uint8_t ata_poll_read28(uint32_t lba, void* buf, uint32_t count) {
    uint16_t* ptr = (uint16_t*)buf;
    while (count) {
        if (!ata_poll_wait_idle()) return 0;

        outb(ATA_REG_DRIVE_SEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
        for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
        outb(ATA_REG_SECCOUNT, 1);
        outb(ATA_REG_LBA_LOW,  (uint8_t)lba);
        outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
        outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
        for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

        if (!ata_poll_wait_drq()) return 0;
        insw(ATA_REG_DATA, ptr, 256);

        ptr += 256;
        lba++;
        count--;
    }
    if (!ata_poll_wait_idle()) return 0;
    return 1;
}

uint8_t ata_poll_write28(uint32_t lba, const void* buf, uint32_t count) {
    const uint16_t* ptr = (const uint16_t*)buf;
    while (count) {
        if (!ata_poll_wait_idle()) return 0;

        outb(ATA_REG_DRIVE_SEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
        for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);
        outb(ATA_REG_SECCOUNT, 1);
        outb(ATA_REG_LBA_LOW,  (uint8_t)lba);
        outb(ATA_REG_LBA_MID,  (uint8_t)(lba >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
        outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
        for (int i = 0; i < 4; i++) inb(ATA_REG_CONTROL);

        if (!ata_poll_wait_drq()) return 0;
        outsw(ATA_REG_DATA, ptr, 256);

        ptr += 256;
        lba++;
        count--;
    }
    if (!ata_poll_wait_idle()) return 0;
    return 1;
}
