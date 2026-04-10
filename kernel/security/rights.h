#pragma once
#include "common.h"

// ── fd Rights Bitmask ─────────────────────────────────────────────────────
//
// Every open vfs_file_t carries a rights bitmask stamped at open time.
// Rights can only flow DOWNWARD: dup/fork copy the same mask; sys_restrict_fd
// ANDs it; sys_sendfd requires new_rights ⊆ existing rights.  The kernel
// checks the relevant bit(s) before executing any fd operation.
//
// These are internal — invisible through the POSIX API surface.  A process
// that received a RIGHT_READ-only fd cannot write regardless of which
// syscall it calls.

#define RIGHT_READ    (1u << 0)   // read()
#define RIGHT_WRITE   (1u << 1)   // write()
#define RIGHT_EXEC    (1u << 2)   // execve() on this fd (O_PATH exec)
#define RIGHT_SEEK    (1u << 3)   // lseek()
#define RIGHT_POLL    (1u << 4)   // poll()/select() readiness query
#define RIGHT_MMAP_R  (1u << 5)   // mmap(PROT_READ)
#define RIGHT_MMAP_W  (1u << 6)   // mmap(PROT_WRITE)
#define RIGHT_MMAP_X  (1u << 7)   // mmap(PROT_EXEC)
#define RIGHT_IOCTL   (1u << 8)   // ioctl()
#define RIGHT_SEND_FD (1u << 9)   // sendfd() — can transfer this fd over a socket

// Convenience composites.
#define RIGHTS_RDONLY  (RIGHT_READ | RIGHT_SEEK | RIGHT_POLL | RIGHT_MMAP_R)
#define RIGHTS_WRONLY  (RIGHT_WRITE | RIGHT_SEEK | RIGHT_POLL)
#define RIGHTS_RDWR    (RIGHTS_RDONLY | RIGHTS_WRONLY | RIGHT_MMAP_W)
#define RIGHTS_ALL     (0x3FFu)   // all 10 bits

// Derive an initial rights mask from O_RDONLY/O_WRONLY/O_RDWR open flags.
// `exec_ok` — set if the inode's execute bit was checked and permitted.
static inline uint32_t rights_from_oflags(int oflags, int exec_ok) {
    uint32_t r = RIGHT_POLL | RIGHT_SEEK;
    int acc = oflags & 3;   // O_RDONLY=0, O_WRONLY=1, O_RDWR=2
    if (acc == 0 || acc == 2) r |= RIGHT_READ  | RIGHT_MMAP_R;
    if (acc == 1 || acc == 2) r |= RIGHT_WRITE | RIGHT_MMAP_W;
    if (exec_ok)              r |= RIGHT_EXEC  | RIGHT_MMAP_X;
    r |= RIGHT_IOCTL | RIGHT_SEND_FD;
    return r;
}

// Restrict: returns `current` AND-ed with `mask`.  Never increases rights.
static inline uint32_t rights_restrict(uint32_t current, uint32_t mask) {
    return current & mask;
}

// Check: returns 1 if `have` satisfies all bits in `need`, 0 otherwise.
static inline int rights_check(uint32_t have, uint32_t need) {
    return (have & need) == need;
}
