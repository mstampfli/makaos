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
typedef unsigned long size_t;

typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef char int8_t;

#ifndef __cplusplus
  typedef unsigned char bool;
  #define true  1
  #define false 0
#else
#endif

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
extern void outl(uint16_t port, uint32_t value);
extern uint32_t inl(uint16_t port);
extern void outsw(uint16_t port, const void* addr, uint32_t count);
extern void insw(uint16_t port, void* addr, uint32_t count);

__attribute__((no_caller_saved_registers)) uint8_t inb_irq(uint16_t port);
__attribute__((no_caller_saved_registers)) void outb_irq(uint16_t port, uint8_t value);
__attribute__((no_caller_saved_registers)) void insw_irq(uint16_t port, void* addr, uint32_t count);
__attribute__((no_caller_saved_registers)) void outsw_irq(uint16_t port, const void* addr, uint32_t count);

// ── Quick serial debug helpers ───────────────────────────────────────────
static inline void serial_putc_dbg(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}
static inline void serial_puts_dbg(const char* s) {
    for (; *s; s++) serial_putc_dbg(*s);
}
static inline void serial_hex_dbg(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (uint8_t)((v >> i) & 0xF);
        serial_putc_dbg(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    serial_putc_dbg('\n');
}

#define KERNEL_CS    0x08   // kernel code segment selector
#define KERNEL_SS    0x10   // kernel data/stack segment selector
#define USER_CS      0x2B   // user code selector  (0x28 | RPL3)
#define USER_SS      0x23   // user data selector  (0x20 | RPL3)
#define PAGE_SHIFT 12  // 12 for 4KB, 21 for 2MB, 30 for 1GB
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define PAGE_MASK  (PAGE_SIZE - 1)
#define VGA_ADDR 0xB8000ULL 
#define UINT64_MAX ((uint64_t)0xFFFFFFFFFFFFFFFFULL)
#define UINT32_MAX 0xFFFFFFFFU

// Maximum kernel path length — matches POSIX PATH_MAX (4096).
// Use this everywhere instead of ad-hoc 256/512 stack buffers.
#define KPATH_MAX   4096u

#define HHDM_OFFSET 0xFFFF800000000000ULL
#define GIB_SIZE    (1ULL << 30)
#define PS_BIT      (1ULL << 7) // Page Size bit
#define KERNEL_BASE_VIRT 0xFFFFFFFF80000000ULL

#define NULL ((void*)0)

typedef struct __attribute__((packed)) boot_info_t {
    uint32_t sig0;   /* 0xB007EF11 */
    uint32_t sig1;   /* 0xCAFEF00D */

    /* Memory map (translated to e820 format by UEFI bootloader) */
    uint16_t      e820_count;
    e820_entry_t  e820_map[E820_MAX];

    /* GOP linear framebuffer */
    uint64_t fb_phys;      /* physical base address */
    uint32_t fb_width;     /* horizontal resolution in pixels */
    uint32_t fb_height;    /* vertical resolution in pixels */
    uint32_t fb_pitch;     /* bytes per scanline */
    uint32_t fb_bpp;       /* bits per pixel (32 for BGRX) */

    /* Kernel placement */
    uint64_t kernel_phys_base;
    uint64_t phys_ceiling;

    /* Page table root built by UEFI bootloader */
    uint64_t pml4_phys;

    /* HHDM offset (constant = 0xFFFF800000000000) */
    uint64_t hhdm_offset;
} boot_info_t;

extern phys_addr_t KERNEL_BASE_PHYS;
extern uint64_t    KERNEL_SIZE;           // actual kernel binary size (from linker symbols)
extern uint64_t    LOADER_RESERVED_SIZE;  // physical region to exclude from PMM:
                                          // covers kernel + loader page tables at top of window

// ── Compiler attributes ─────────────────────────────────────────────────
// ALWAYS_INLINE forces inlining regardless of optimization level.  Use for
// hot-path helpers (preempt_disable, rcu_read_lock, atomic_*, spinlocks)
// so -O0 debug builds keep them correct and -O2 builds keep them fast
// without depending on the inliner's heuristics.
#define ALWAYS_INLINE static inline __attribute__((always_inline))

// NOINLINE prevents inlining even under -O2.  Use for cold slow paths
// (error handlers, panic) so the hot caller doesn't grow bloated.
#define NOINLINE __attribute__((noinline))

// Branch prediction hints.
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
