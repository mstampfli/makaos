#pragma once

extern char __bss_start[];
extern char __bss_end[];

extern char __kernel_start[];
extern char __kernel_end[];

typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef unsigned char  uint8_t;
typedef uint64_t uintptr_t;
typedef uintptr_t virt_addr_t;
typedef uint64_t phys_addr_t;

typedef struct e820_entry_t {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
}__attribute__((packed)) e820_entry_t;

#define E820_MAX 128

// arg 1 goes to rdi, arg 2 to rsi, arg 3 to rdx
extern void outb(uint16_t port, uint8_t value);
extern uint8_t inb(uint16_t port);
extern void outw(uint16_t port, uint16_t value);
extern uint16_t inw(uint16_t port);
extern void outsw(uint16_t port, const void* addr, uint32_t count);
extern void insw(uint16_t port, void* addr, uint32_t count);
extern void outl(uint16_t port, uint32_t value);
extern uint32_t inl(uint16_t port);

__attribute__((no_caller_saved_registers)) uint8_t inb_irq(uint16_t port);
__attribute__((no_caller_saved_registers)) void outb_irq(uint16_t port, uint8_t value);
__attribute__((no_caller_saved_registers)) void insw_irq(uint16_t port, void* addr, uint32_t count);
__attribute__((no_caller_saved_registers)) void outsw_irq(uint16_t port, const void* addr, uint32_t count);

#define KERNEL_CS 0x18
#define PAGE_SHIFT 12                 // 12 for 4KB, 21 for 2MB, 30 for 1GB
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define PAGE_MASK  (PAGE_SIZE - 1)
#define VGA_ADDR 0xB8000ULL 
#define UINT64_MAX ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

#define HHDM_OFFSET 0xFFFF800000000000ULL
#define GIB_SIZE    (1ULL << 30)
#define PS_BIT      (1ULL << 7) // Page Size bit
#define KERNEL_BASE_VIRT 0xFFFFFFFF80000000ULL

#define NULL ((void*)0)

/*typedef struct boot_info_t {
    uint16_t     e820_count;
    e820_entry_t e820_map[E820_MAX]; // 64 * 24 = 1536 bytes

    uint16_t vbe_mode;
    uint16_t vbe_w;
    uint16_t vbe_h;
    uint16_t vbe_pitch;
    uint8_t  vbe_bpp;
    uint8_t  _pad0;        // Matches 'align 4' logic
    uint32_t vbe_fb;

    uint8_t  vbe_mode_info[256]; // The 256-byte buffer from your ASM

    // --- NEW FIELDS (Place these AFTER the 256-byte buffer) ---
    uint64_t kernel_phys_base;
    uint64_t phys_ceiling;
    uint64_t hhdm_offset;
    uint64_t pml4_phys;
} __attribute__((packed)) boot_info_t;*/

typedef struct __attribute__((packed)) boot_info_t {
    // signature written by boot2 at runtime
    uint32_t sig0; // 0x11112222
    uint32_t sig1; // 0x33334444
    uint32_t sig2; // 0x55556666
    uint32_t sig3; // 0x77778888

    uint16_t e820_dbg_flags_before;
    uint16_t e820_dbg_es;
    uint16_t e820_dbg_di;

    uint32_t e820_dbg_eax_before;
    uint32_t e820_dbg_ebx_before;
    uint32_t e820_dbg_ecx_before;
    uint32_t e820_dbg_edx_before;

    uint16_t e820_dbg_flags_after;
    uint8_t  e820_dbg_cf_after;
    uint8_t  _pad1;

    uint32_t e820_dbg_eax_after;
    uint32_t e820_dbg_ebx_after;
    uint32_t e820_dbg_ecx_after;
    uint32_t e820_dbg_edx_after;

    uint16_t e820_count;
    e820_entry_t e820_map[E820_MAX];

    uint16_t vbe_dbg_ax_4f01;
    uint16_t vbe_dbg_ax_4f02;

    uint16_t vbe_mode;
    uint16_t vbe_w;
    uint16_t vbe_h;
    uint16_t vbe_pitch;
    uint8_t  vbe_bpp;
    uint8_t  _pad0;
    uint32_t vbe_fb;
    uint8_t  vbe_mode_info[256];

    uint64_t kernel_phys_base;
    uint64_t phys_ceiling;
    uint64_t hhdm_offset;
    uint64_t pml4_phys;
} boot_info_t;
