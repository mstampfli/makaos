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
//
// These wrap a raw x86 16550 UART at port 0x3F8.  The port is a single
// serialised hardware register, so concurrent access from multiple CPUs
// corrupts both the TX FIFO and the LSR-bit-5 busy-wait.  We serialise
// writers with a self-contained spinlock that does NOT depend on smp.h
// (common.h is too low in the include tree to pull in spinlock_t).
//
// Locking discipline:
//   - Disable IRQs on this CPU first (cli).  That stops any IRQ handler
//     from re-entering serial and self-deadlocking on the same CPU.
//   - Acquire the global lock via atomic xchg — one cache-line op.
//   - Drain the whole message inline; the LSR-poll + outb stays as-is
//     and runs under the lock so two CPUs never interleave bytes.
//   - Release the lock, restore the prior IRQ flag.
//
// The lock is valid from very early boot (plain BSS-zero initial state)
// through panic (panic path takes the lock; worst case it spins briefly
// if another CPU was mid-write, then proceeds).
//
// Performance: the UART itself is the bottleneck (port I/O is slow).
// Adding one xchg per message is noise next to the port write.  Per-
// byte locking would be wasteful — the helpers below take the lock ONCE
// per call so the whole "[net] rx\n" goes out under a single critical
// section.

extern volatile uint32_t g_serial_lock;

static inline uint64_t serial_lock_irqsave(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags));
    uint32_t zero = 0;
    while (!__atomic_compare_exchange_n(&g_serial_lock, &zero, 1u, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        zero = 0;
        while (__atomic_load_n(&g_serial_lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
    return rflags;
}

static inline void serial_unlock_irqrestore(uint64_t rflags) {
    __atomic_store_n(&g_serial_lock, 0u, __ATOMIC_RELEASE);
    if (rflags & (1ull << 9)) __asm__ volatile("sti");
}

// Raw unlocked primitive — only callable while holding the serial lock,
// or from early boot / panic paths where the system is already dead.
static inline void serial_raw_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}

// ── Debug serial output — gated by KERNEL_DEBUG_SERIAL ──────────────────
//
// serial_*_dbg helpers write to port 0x3F8 under g_serial_lock.  Cheap
// individually, but scattered through the syscall dispatch (spawn,
// socket, bind, listen, accept, connect, shm_open, fb_map, …), every
// app startup or connect() goes through dozens of them.  At ~10 KB/s
// serial bandwidth, that alone makes ps scroll visibly.
//
// KERNEL_DEBUG_SERIAL = 0 compiles every serial_*_dbg call to a no-op;
// call sites are still valid syntax, but produce zero instructions.
// Flip to 1 when you actively want kernel trace in serial.txt.
//
// This gate does NOT silence the PF-KILL handler in vmm.c, which uses
// its own local ser_str / ser_hex64 helpers — those are always on
// because they're the only diagnostic we get from a fatal crash.
#define KERNEL_DEBUG_SERIAL 0

#if KERNEL_DEBUG_SERIAL
static inline void serial_putc_dbg(char c) {
    uint64_t f = serial_lock_irqsave();
    serial_raw_putc(c);
    serial_unlock_irqrestore(f);
}

static inline void serial_puts_dbg(const char* s) {
    uint64_t f = serial_lock_irqsave();
    for (; *s; s++) serial_raw_putc(*s);
    serial_unlock_irqrestore(f);
}

static inline void serial_hex_dbg(uint64_t v) {
    uint64_t f = serial_lock_irqsave();
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (uint8_t)((v >> i) & 0xF);
        serial_raw_putc(n < 10 ? '0' + n : 'A' + (n - 10));
    }
    serial_raw_putc('\n');
    serial_unlock_irqrestore(f);
}
#else
static inline void serial_putc_dbg(char c)         { (void)c; }
static inline void serial_puts_dbg(const char* s)  { (void)s; }
static inline void serial_hex_dbg(uint64_t v)      { (void)v; }
#endif

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

// x86-64 user address space is [0, 2^47) under 4-level paging.  Everything at
// or above 2^47 is either kernel or the non-canonical gap up to HHDM_OFFSET,
// which must be rejected: a non-canonical pointer #GPs the instant the CPU
// ring-transitions to it via iretq (the Ctrl+C makaterm crash).  Two spellings
// of the ONE boundary -- use whichever reads naturally with the comparison:
//   USER_ADDR_MAX  = last valid user byte     (inclusive; pair with >  or <=)
//   USER_ADDR_CEIL = first non-canonical addr (exclusive; pair with >= or < )
// Invariant: USER_ADDR_CEIL == USER_ADDR_MAX + 1 == 2^47.
#define USER_ADDR_MAX  0x00007FFFFFFFFFFFULL
#define USER_ADDR_CEIL 0x0000800000000000ULL

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

    /* ACPI RSDP physical address — discovered by the UEFI bootloader
     * from the EFI System Table's configuration tables.  0 if not
     * found (kernel falls back to scanning the legacy BIOS area).
     * Required for Phase 9 SMP: the MADT is reachable only via the
     * RSDP → XSDT → MADT chain, and UEFI-booted systems don't leave
     * the RSDP in the legacy 0xE0000-0xFFFFF region. */
    uint64_t rsdp_phys;
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

// ── kmemcmp / kmemeq — fast in-kernel byte comparison ──────────────────
// The freestanding build has no libc memcmp, and clang occasionally
// lowers __builtin_memcmp to a libc call for variable-length inputs.
// Instead of a byte-loop (~1 compare/cycle) we emit `repe cmpsb`,
// which on modern x86 with Fast Short REP (Ice Lake+, Zen 3+) runs
// ~8 bytes/cycle and is microcode-accelerated for short strings
// (path names, hash keys).  The equality-only variant (kmemeq)
// returns 1/0 — slightly cheaper than kmemcmp since we don't need
// to compute the sign of the difference.
ALWAYS_INLINE int kmemcmp(const void* a, const void* b, uint64_t n) {
    uint64_t flags;
    __asm__ volatile(
        "repe cmpsb\n\t"
        "pushfq\n\t"
        "pop %0"
        : "=r"(flags), "+D"(a), "+S"(b), "+c"(n)
        :
        : "memory", "cc");
    // ZF set ⇒ all bytes equal (return 0); ZF clear ⇒ differ.
    // Last (differing) bytes are at [*a - 1], [*b - 1] via RDI/RSI.
    // For an equality check, caller uses kmemeq.  For the full
    // negative/positive ordering we compare the last byte pair.
    if (flags & 0x40) return 0;     // ZF bit 6 — all equal
    const uint8_t* pa = ((const uint8_t*)a) - 1;
    const uint8_t* pb = ((const uint8_t*)b) - 1;
    return (int)*pa - (int)*pb;
}

ALWAYS_INLINE int kmemeq(const void* a, const void* b, uint64_t n) {
    uint8_t eq;
    __asm__ volatile(
        "repe cmpsb\n\t"
        "setz %0"
        : "=q"(eq), "+D"(a), "+S"(b), "+c"(n)
        :
        : "memory", "cc");
    return (int)eq;
}
