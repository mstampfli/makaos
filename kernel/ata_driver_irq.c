#include "ata_driver_irq.h"
#include "common.h"

static ata_context_t g_ata_ctx = {
    .irq = PIC_IRQ_ATA_PRIMARY,    // IRQ14
    .multiple = 1,                 // 16 sectors per DRQ block
    .irq_fired = 0,
    .last_status = 0,
    .op = ATA_OP_NONE,
    .step = ATA_STEP_IDLE,
    .delay_400 = 0,
    .lba = 0,
    .sectors_left = 0,
    .batch_left = 0,
    .ptr = 0
};

static inline uint8_t ata_status_read_once(void) {
    // STATUS (0x1F7): reading clears the device INTRQ condition
    return inb(ATA_REG_STATUS);
}

static inline void pic_io_wait(void) {
    // small I/O delay for PIC programming
    outb(0x80, 0);
}

static inline uint8_t ata_alt_status_read_once(void) {
    // CONTROL (0x3F6): returns alternate status and does not clear INTRQ
    return inb(ATA_REG_CONTROL);
}

static inline uint8_t ata_status_is_bad(uint8_t st) {
    if (st == 0xFF) return 1;
    if (st & (ATA_SR_ERR | ATA_SR_DF)) return 1;
    return 0;
}

void remap_pic(uint8_t master_offset, uint8_t slave_offset) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask  = inb(PIC2_DATA);

    // ICW1: Init sequence + ICW4 expected
    outb(PIC1_CMD, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    pic_io_wait();
    outb(PIC2_CMD, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    pic_io_wait();

    // ICW2: Vector offsets
    outb(PIC1_DATA, master_offset);
    pic_io_wait();
    outb(PIC2_DATA, slave_offset);
    pic_io_wait();

    // ICW3: Cascade setup
    outb(PIC1_DATA, 0x04);
    pic_io_wait();
    outb(PIC2_DATA, 0x02);
    pic_io_wait();

    // ICW4: 8086 mode
    outb(PIC1_DATA, PIC_ICW4_8086);
    pic_io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);
    pic_io_wait();

    // restore masks
    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

void unmask_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t mask = inb(PIC1_DATA);
        mask = (uint8_t)(mask & (uint8_t)~(1u << irq));
        outb(PIC1_DATA, mask);
        return;
    }

    uint8_t mask2 = inb(PIC2_DATA);
    mask2 = (uint8_t)(mask2 & (uint8_t)~(1u << (irq - 8)));
    outb(PIC2_DATA, mask2);

    uint8_t mask1 = inb(PIC1_DATA);
    mask1 = (uint8_t)(mask1 & (uint8_t)~(1u << PIC_IRQ_CASCADE));
    outb(PIC1_DATA, mask1);
}

__attribute__((no_caller_saved_registers))
void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb_irq(PIC2_CMD, PIC_CMD_EOI);
    }
    outb_irq(PIC1_CMD, PIC_CMD_EOI);
}

void setup_ata_irq(void) {
    remap_pic(PIC_REMAP_MASTER, PIC_REMAP_SLAVE);
    unmask_irq(PIC_IRQ_ATA_PRIMARY);
}

static inline void ata_delay_400ns_start(void) {
    g_ata_ctx.delay_400 = 4;
}

static inline uint8_t ata_delay_400ns_tick(void) {
    if (g_ata_ctx.delay_400 == 0) return 1;
    (void)inb(ATA_REG_CONTROL);
    g_ata_ctx.delay_400--;
    return (g_ata_ctx.delay_400 == 0);
}

__attribute__((interrupt, no_caller_saved_registers))
void irq14_ata(interrupt_frame_t* f) {
    (void)f;
    g_ata_ctx.last_status = inb_irq(ATA_REG_STATUS);
    g_ata_ctx.irq_fired = 1;
    send_eoi(PIC_IRQ_ATA_PRIMARY);
}

uint8_t is_ata_irq_busy(void) {
    if (g_ata_ctx.step == ATA_STEP_IDLE || 
        g_ata_ctx.step == ATA_STEP_INIT_DONE || g_ata_ctx.step == ATA_STEP_INIT_FAIL ||
        g_ata_ctx.step == ATA_STEP_IO_DONE || g_ata_ctx.step == ATA_STEP_IO_FAIL) return 0;
    return 1;
}

uint8_t is_ata_irq_done(void) { return (g_ata_ctx.step == ATA_STEP_IO_DONE); }
uint8_t is_ata_irq_failed(void) { return (g_ata_ctx.step == ATA_STEP_IO_FAIL); }
uint8_t is_ata_init_done(void) { return (g_ata_ctx.step == ATA_STEP_INIT_DONE); }
uint8_t is_ata_init_failed(void) { return (g_ata_ctx.step == ATA_STEP_INIT_FAIL); }

static inline void ata_irq_flags_reset(void) {
    g_ata_ctx.irq_fired = 0;
    g_ata_ctx.last_status = 0;
}

static inline void ata_request_state_reset(void) {
    g_ata_ctx.delay_400 = 0;
    ata_irq_flags_reset();
}

uint8_t begin_init_ata_28_irq(void) {
    if (g_ata_ctx.step != ATA_STEP_IDLE) return 0;
    g_ata_ctx.op = ATA_OP_INIT_28;
    g_ata_ctx.step = ATA_STEP_INIT_SELECT;
    ata_request_state_reset();
    return 1;
}

uint8_t begin_init_ata_48_irq(void) {
    if (g_ata_ctx.step != ATA_STEP_IDLE) return 0;
    g_ata_ctx.op = ATA_OP_INIT_48;
    g_ata_ctx.step = ATA_STEP_INIT_SELECT;
    ata_request_state_reset();
    return 1;
}

uint8_t begin_read_from_disk_ata_28_irq(uint32_t lba, void* addr, uint32_t sector_count) {
    if (g_ata_ctx.step != ATA_STEP_IDLE || sector_count == 0) return 0;
    g_ata_ctx.op = ATA_OP_READ_28;
    g_ata_ctx.step = ATA_STEP_IO_ISSUE_SELECT;
    g_ata_ctx.lba = (uint64_t)lba;
    g_ata_ctx.sectors_left = sector_count;
    g_ata_ctx.batch_left = 0;
    g_ata_ctx.ptr = (uint16_t*)addr;
    ata_request_state_reset();
    return 1;
}

uint8_t begin_write_to_disk_ata_28_irq(uint32_t lba, void* addr, uint32_t sector_count) {
    if (g_ata_ctx.step != ATA_STEP_IDLE || sector_count == 0) return 0;
    g_ata_ctx.op = ATA_OP_WRITE_28;
    g_ata_ctx.step = ATA_STEP_IO_ISSUE_SELECT;
    g_ata_ctx.lba = (uint64_t)lba;
    g_ata_ctx.sectors_left = sector_count;
    g_ata_ctx.batch_left = 0;
    g_ata_ctx.ptr = (uint16_t*)addr;
    ata_request_state_reset();
    return 1;
}

uint8_t begin_read_from_disk_ata_48_irq(uint64_t lba, void* addr, uint32_t sector_count) {
    if (g_ata_ctx.step != ATA_STEP_IDLE || sector_count == 0) return 0;
    g_ata_ctx.op = ATA_OP_READ_48;
    g_ata_ctx.step = ATA_STEP_IO_ISSUE_SELECT;
    g_ata_ctx.lba = lba;
    g_ata_ctx.sectors_left = sector_count;
    g_ata_ctx.batch_left = 0;
    g_ata_ctx.ptr = (uint16_t*)addr;
    ata_request_state_reset();
    return 1;
}

uint8_t begin_write_to_disk_ata_48_irq(uint64_t lba, void* addr, uint32_t sector_count) {
    if (g_ata_ctx.step != ATA_STEP_IDLE || sector_count == 0) return 0;
    g_ata_ctx.op = ATA_OP_WRITE_48;
    g_ata_ctx.step = ATA_STEP_IO_ISSUE_SELECT;
    g_ata_ctx.lba = lba;
    g_ata_ctx.sectors_left = sector_count;
    g_ata_ctx.batch_left = 0;
    g_ata_ctx.ptr = (uint16_t*)addr;
    ata_request_state_reset();
    return 1;
}

uint8_t initialize_ata_28_irq(void) { return begin_init_ata_28_irq(); }

static inline void ata_finish_io(uint8_t success) {
    g_ata_ctx.step = success ? ATA_STEP_IO_DONE : ATA_STEP_IO_FAIL;
    g_ata_ctx.op = ATA_OP_NONE;
}

static inline void ata_finish_init(uint8_t success) {
    g_ata_ctx.step = success ? ATA_STEP_INIT_DONE : ATA_STEP_INIT_FAIL;
    g_ata_ctx.op = ATA_OP_NONE;
}

static inline void ata_init_select_handle(void) {
    outb(ATA_REG_CONTROL, ATA_CTRL_IRQ_ENABLE);
    outb(ATA_REG_DRIVE_SEL, ATA_DH_LBA_MASTER);
    ata_delay_400ns_start();
    g_ata_ctx.step = ATA_STEP_INIT_IDENTIFY_CMD;
}

static inline void ata_init_identify_cmd_handle(void) {
    outb(ATA_REG_SECCOUNT, 0);
    outb(ATA_REG_LBA_LOW,  0);
    outb(ATA_REG_LBA_MID,  0);
    outb(ATA_REG_LBA_HIGH, 0);
    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_irq_flags_reset();
    g_ata_ctx.step = ATA_STEP_INIT_IDENTIFY_WAIT;
}

static inline void ata_init_identify_wait_handle(void) {
    if (!g_ata_ctx.irq_fired) return;
    g_ata_ctx.irq_fired = 0;

    uint8_t st = g_ata_ctx.last_status;
    if (ata_status_is_bad(st)) { ata_finish_init(0); return; }

    if (st & ATA_SR_DRQ) { g_ata_ctx.step = ATA_STEP_INIT_IDENTIFY_XFER; return; }

    st = ata_alt_status_read_once();
    if (ata_status_is_bad(st)) { ata_finish_init(0); return; }
    if (st & ATA_SR_DRQ) g_ata_ctx.step = ATA_STEP_INIT_IDENTIFY_XFER;
}

static inline void ata_init_identify_xfer_handle(void) {
    insw(ATA_REG_DATA, g_ata_ctx.identify, 256);
    ata_finish_init(1);
}

static inline void ata_init_tick(void) {
    switch (g_ata_ctx.step) {
        case ATA_STEP_INIT_SELECT:        ata_init_select_handle();        return;
        case ATA_STEP_INIT_IDENTIFY_CMD:  ata_init_identify_cmd_handle();  return;
        case ATA_STEP_INIT_IDENTIFY_WAIT: ata_init_identify_wait_handle(); return;
        case ATA_STEP_INIT_IDENTIFY_XFER: ata_init_identify_xfer_handle(); return;
        default: return;
    }
}

static inline void ata_io_issue_select_handle(void) {
    if (g_ata_ctx.op == ATA_OP_READ_28 || g_ata_ctx.op == ATA_OP_WRITE_28) {
        uint32_t lba28 = (uint32_t)g_ata_ctx.lba;
        uint8_t top4 = (uint8_t)((lba28 >> 24) & 0x0F);
        outb(ATA_REG_DRIVE_SEL, (uint8_t)(ATA_DH_LBA_MASTER | top4));
    } else {
        outb(ATA_REG_DRIVE_SEL, ATA_DH_LBA_MASTER);
    }
    ata_delay_400ns_start();
    g_ata_ctx.step = ATA_STEP_IO_ISSUE_TASKFILE;
}

static inline void ata_io_issue_taskfile_handle(void) {
    if (g_ata_ctx.sectors_left == 0) { ata_finish_io(1); return; }
    g_ata_ctx.batch_left = 1;

    if (g_ata_ctx.op == ATA_OP_READ_28 || g_ata_ctx.op == ATA_OP_WRITE_28) {
        uint32_t lba28 = (uint32_t)g_ata_ctx.lba;
        outb(ATA_REG_SECCOUNT, 1);
        outb(ATA_REG_LBA_LOW,  (uint8_t)lba28);
        outb(ATA_REG_LBA_MID,  (uint8_t)(lba28 >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(lba28 >> 16));
        outb(ATA_REG_COMMAND, (g_ata_ctx.op == ATA_OP_READ_28) ? ATA_CMD_READ_SECTORS : ATA_CMD_WRITE_SECTORS);
    } else {
        outb(ATA_REG_SECCOUNT, 0); // High byte
        outb(ATA_REG_LBA_LOW,  (uint8_t)(g_ata_ctx.lba >> 24));
        outb(ATA_REG_LBA_MID,  (uint8_t)(g_ata_ctx.lba >> 32));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(g_ata_ctx.lba >> 40));
        outb(ATA_REG_SECCOUNT, 1); // Low byte
        outb(ATA_REG_LBA_LOW,  (uint8_t)g_ata_ctx.lba);
        outb(ATA_REG_LBA_MID,  (uint8_t)(g_ata_ctx.lba >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(g_ata_ctx.lba >> 16));
        outb(ATA_REG_COMMAND, (g_ata_ctx.op == ATA_OP_READ_48) ? ATA_CMD_READ_SECTORS_EXT : ATA_CMD_WRITE_SECTORS_EXT);
    }
    ata_irq_flags_reset();
    g_ata_ctx.step = ATA_STEP_IO_WAIT_IRQ;
}

static inline void ata_io_wait_irq_handle(void) {
    uint8_t st;
    if (g_ata_ctx.irq_fired) {
        g_ata_ctx.irq_fired = 0;
        st = g_ata_ctx.last_status;
    } else {
        if (g_ata_ctx.batch_left != 0) return;
        st = ata_alt_status_read_once();
    }

    if (ata_status_is_bad(st)) { ata_finish_io(0); return; }
    if (st & ATA_SR_DRQ) { g_ata_ctx.step = ATA_STEP_IO_XFER_BLOCK; return; }
    if (g_ata_ctx.batch_left == 0) {
        if (g_ata_ctx.sectors_left == 0) { ata_finish_io(1); return; }
        g_ata_ctx.step = ATA_STEP_IO_ISSUE_SELECT;
        return;
    }
    st = ata_alt_status_read_once();
    if (ata_status_is_bad(st)) { ata_finish_io(0); return; }
    if (st & ATA_SR_DRQ) g_ata_ctx.step = ATA_STEP_IO_XFER_BLOCK;
}

static inline void ata_io_xfer_block_handle(void) {
    uint32_t n = 1;
    if (g_ata_ctx.batch_left < n) n = g_ata_ctx.batch_left;
    if (n == 0) { g_ata_ctx.step = ATA_STEP_IO_WAIT_IRQ; return; }

    uint32_t word_count = n * 256;
    if (g_ata_ctx.op == ATA_OP_READ_28 || g_ata_ctx.op == ATA_OP_READ_48) {
        insw(ATA_REG_DATA, g_ata_ctx.ptr, word_count);
    } else {
        outsw(ATA_REG_DATA, g_ata_ctx.ptr, word_count);
    }

    g_ata_ctx.ptr += word_count;
    g_ata_ctx.lba += n;
    g_ata_ctx.batch_left -= n;
    g_ata_ctx.sectors_left -= n;
    g_ata_ctx.step = ATA_STEP_IO_WAIT_IRQ;
}

static inline void ata_io_tick(void) {
    switch (g_ata_ctx.step) {
        case ATA_STEP_IO_ISSUE_SELECT:   ata_io_issue_select_handle();   return;
        case ATA_STEP_IO_ISSUE_TASKFILE: ata_io_issue_taskfile_handle(); return;
        case ATA_STEP_IO_WAIT_IRQ:       ata_io_wait_irq_handle();       return;
        case ATA_STEP_IO_XFER_BLOCK:     ata_io_xfer_block_handle();     return;
        default: return;
    }
}

void tick_ata(void) {
    if (g_ata_ctx.delay_400 != 0) {
        (void)ata_delay_400ns_tick();
        if (g_ata_ctx.delay_400 != 0) return;
    }
    if (g_ata_ctx.op == ATA_OP_INIT_28 || g_ata_ctx.op == ATA_OP_INIT_48) {
        ata_init_tick();
    } else {
        ata_io_tick();
    }
}
