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
#define SYS_FORK    7   // fork()                      → child pid in parent, 0 in child
#define SYS_EXEC    8   // exec(name, len)             → doesn't return on success
#define SYS_WAIT    9   // wait(pid)                   → 0 when child exits
#define SYS_GETPID  10  // getpid()                    → current pid
#define SYS_READDIR 11  // readdir(buf, max)           → entry count
#define SYS_SPAWN   12  // spawn(path, pathlen)        → child pid, or -1
#define SYS_THREAD  13  // thread(entry, stack_top)    → tid, or -1
#define SYS_CLOCK_NS 14 // clock_ns()                  → nanoseconds since boot

#define SYS_READ_NONBLOCK 1

// ── Calling convention (System V AMD64, mirroring Linux) ─────────────────
// rax = syscall number
// rdi = arg1, rsi = arg2, rdx = arg3
// rcx and r11 are clobbered by syscall/sysret (CPU saves user rip/rflags there)
// return value in rax

// ── Globals exposed to syscall_entry.asm ─────────────────────────────────
extern uint64_t    g_syscall_user_rsp;
extern uint64_t    g_syscall_user_rip;
extern uint64_t    g_syscall_user_rflags;
extern uint8_t     g_exec_requested;
extern uint64_t    g_exec_entry;
extern uint64_t    g_exec_rsp;
extern phys_addr_t g_exec_pml4;

// ── Init ──────────────────────────────────────────────────────────────────
// Write STAR/LSTAR/SFMASK MSRs and enable SCE in EFER.
// Call once after tss_init().
void syscall_init(void);

// ── Dispatcher (called from assembly stub) ────────────────────────────────
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// ── Entry point (defined in syscall_entry.asm) ────────────────────────────
extern void syscall_entry(void);
