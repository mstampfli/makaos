#pragma once
#include "common.h"
#include "idt.h"

typedef enum ata_op_t {
  ATA_OP_NONE = 0,

  ATA_OP_INIT_28,
  ATA_OP_INIT_48,

  ATA_OP_READ_28,
  ATA_OP_WRITE_28,

  ATA_OP_READ_48,
  ATA_OP_WRITE_48
} ata_op_t;

typedef enum ata_step_t {
  ATA_STEP_IDLE = 0,

  // init flow (IRQ driven)
  ATA_STEP_INIT_SELECT,        // wrote DRIVE_SEL, now need 400ns delay ticks
  ATA_STEP_INIT_IDENTIFY_CMD,  // write IDENTIFY
  ATA_STEP_INIT_IDENTIFY_WAIT, // wait for IRQ, then read status
  ATA_STEP_INIT_IDENTIFY_XFER, // if DRQ, read 256 words from DATA
  ATA_STEP_INIT_SETMULT_CMD,   // write SECCOUNT=multi, issue SET_MULTIPLE
  ATA_STEP_INIT_SETMULT_WAIT,  // wait for IRQ completion
  ATA_STEP_INIT_DONE,
  ATA_STEP_INIT_FAIL,

  // I/O flow (IRQ driven)
  ATA_STEP_IO_ISSUE_SELECT,    // write DRIVE_SEL, start 400ns delay
  ATA_STEP_IO_ISSUE_TASKFILE,  // write LBA/count + issue READ/WRITE cmd
  ATA_STEP_IO_ISSUE_BATCH,     // program LBA/count + issue READ/WRITE cmd
  ATA_STEP_IO_WAIT_IRQ,        // wait IRQ and check status
  ATA_STEP_IO_XFER_BLOCK,      // if DRQ, transfer one block (<=multi sectors)
  ATA_STEP_IO_WAIT_DONE,       // optional final completion IRQ
  ATA_STEP_IO_DONE,
  ATA_STEP_IO_FAIL
} ata_step_t;

typedef struct ata_context_t {
  // config (single drive now, still helps later)
  uint8_t irq;          // 14
  uint8_t multiple;     // 16 sectors per DRQ block

  // IRQ communication (must be volatile because IRQ handler writes it)
  volatile uint8_t irq_fired;
  volatile uint8_t last_status;

  // state machine
  ata_op_t op;
  ata_step_t step;

  // non-blocking 400ns delay handling:
  // instead of "for (i=0;i<4;i++) inb(CTRL)" in one shot,
  // you do one CTRL read per tick until delay_400 == 0.
  uint8_t delay_400;

  // in-flight request progress
  uint64_t lba;
  uint32_t sectors_left;    // remaining sectors overall
  uint32_t batch_left;      // remaining sectors in current command batch
  uint16_t* ptr;            // current RAM pointer (word pointer)

  // optional: identify buffer for init
  uint16_t identify[256];
} ata_context_t;

#define ATA_STATUS_IS_FLOATING(st) ((st) == 0xFF)
#define ATA_STATUS_HAS_ERROR(st)   (((st) & (ATA_SR_ERR | ATA_SR_DF)) != 0)
#define ATA_STATUS_HAS_DRQ(st)     (((st) & ATA_SR_DRQ) != 0)
#define ATA_STATUS_IS_BUSY(st)     (((st) & ATA_SR_BSY) != 0)

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

// lba48
#define ATA_CMD_READ_MULTIPLE_EXT 0x29
#define ATA_CMD_WRITE_MULTIPLE_EXT 0x39

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

// Drive/Head (base+6) bits
#define ATA_DH_BASE    0xA0 // 1010 0000: mandatory pattern bits (CHS default)
#define ATA_DH_LBA     0x40 // LBA mode bit
#define ATA_DH_MASTER  0x00 // bit4=0
#define ATA_DH_SLAVE   0x10 // bit4=1 (not used by you)
                            //
#define ATA_DH_LBA_MASTER  (ATA_DH_BASE | ATA_DH_LBA | ATA_DH_MASTER) // usually 0xE0

// ATA commands you use
#define ATA_CMD_IDENTIFY            0xEC

#define ATA_CMD_SET_MULTIPLE        0xC6

#define ATA_CMD_READ_MULTIPLE       0xC4
#define ATA_CMD_WRITE_MULTIPLE      0xC5

#define ATA_CMD_READ_MULTIPLE_EXT   0x29
#define ATA_CMD_WRITE_MULTIPLE_EXT  0x39

#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30
#define ATA_CMD_READ_SECTORS_EXT    0x24
#define ATA_CMD_WRITE_SECTORS_EXT   0x34

#define ATA_MAX_SECTORS_28  256
#define ATA_MAX_SECTORS_48  65536

// 8259 PIC I/O ports                                     
#define PIC1_CMD     0x20   // Master PIC command port
#define PIC1_DATA    0x21   // Master PIC data (mask) port
#define PIC2_CMD     0xA0   // Slave PIC command port
#define PIC2_DATA    0xA1   // Slave PIC data (mask) port

// EOI = "End Of Interrupt" command value
#define PIC_CMD_EOI  0x20

// ICW1 bits (init sequence)
#define PIC_ICW1_INIT 0x10  // Start initialization sequence
#define PIC_ICW1_ICW4 0x01  // ICW4 will be sent

// ICW4 bits
#define PIC_ICW4_8086 0x01  // 8086/88 mode (what you want)

// Default remap offsets (classic)
#define PIC_REMAP_MASTER 0x20
#define PIC_REMAP_SLAVE  0x28

// IRQ numbers you care about
#define PIC_IRQ_CASCADE 2    // Master IRQ2 is where the slave is connected
#define PIC_IRQ_ATA_PRIMARY 14

// initializes ata controller and sets multiple mode
uint8_t initialize_ata_28_irq(void);

// IRQ ATA usage:
// 1) remap PIC + unmask IRQ14: setup_ata_irq()
// 2) IDT vector 46 -> irq14_ata
// 3) sti
// 4) begin_init_ata_28_irq()  (or 48)
// 5) call tick_ata() every kernel loop
// 6) begin_read/write...irq(), keep ticking until done/failed

// PIC setup for IRQ14 -> vector 46, unmask IRQ14
void setup_ata_irq(void);

// IRQ handler (installed in IDT vector 46)
__attribute__((interrupt, no_caller_saved_registers)) void irq14_ata(interrupt_frame_t* f);

// state machine tick (call every kernel loop)
void tick_ata(void);

// init begin (non-blocking)
uint8_t begin_init_ata_28_irq(void);
uint8_t begin_init_ata_48_irq(void);

// I/O begin (non-blocking)
uint8_t begin_read_from_disk_ata_28_irq(uint32_t lba, void* addr, uint32_t sector_count);
uint8_t begin_write_to_disk_ata_28_irq(uint32_t lba, void* addr, uint32_t sector_count);

uint8_t begin_read_from_disk_ata_48_irq(uint64_t lba, void* addr, uint32_t sector_count);
uint8_t begin_write_to_disk_ata_48_irq(uint64_t lba, void* addr, uint32_t sector_count);

// query state (so kernel can know when finished)
uint8_t is_ata_irq_busy(void);
uint8_t is_ata_irq_done(void);
uint8_t is_ata_irq_failed(void);
uint8_t is_ata_init_done(void);
uint8_t is_ata_init_failed(void);

void remap_pic(uint8_t master_offset, uint8_t slave_offset);
void unmask_irq(uint8_t irq);
__attribute__((no_caller_saved_registers))
void send_eoi(uint8_t irq);
