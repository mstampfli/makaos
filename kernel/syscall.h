#pragma once
#include "common.h"

// ── Syscall numbers ───────────────────────────────────────────────────────
#define SYS_WRITE   0   // write(fd, buf, len)        → bytes written
#define SYS_EXIT    1   // exit(code)                  → does not return
#define SYS_READ    2   // read(fd, buf, len, flags)   → bytes read
#define SYS_OPEN    3   // open(path, len)             → fd, or -1 on error
#define SYS_CLOSE   4   // close(fd)                   → 0 or -1
#define SYS_BRK     5   // brk(new_brk)               → new brk, or -1 on error
#define SYS_KILL    6   // kill(pid, sig)              → 0 or -1

#define SYS_READ_NONBLOCK 1

// ── Calling convention (System V AMD64, mirroring Linux) ─────────────────
// rax = syscall number
// rdi = arg1, rsi = arg2, rdx = arg3
// rcx and r11 are clobbered by syscall/sysret (CPU saves user rip/rflags there)
// return value in rax

// ── Init ──────────────────────────────────────────────────────────────────
// Write STAR/LSTAR/SFMASK MSRs and enable SCE in EFER.
// Call once after tss_init().
void syscall_init(void);

// ── Dispatcher (called from assembly stub) ────────────────────────────────
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// ── Entry point (defined in syscall_entry.asm) ────────────────────────────
extern void syscall_entry(void);
