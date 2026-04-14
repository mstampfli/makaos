#pragma once
#include "common.h"

// ── pledge() — Irrevocable Syscall Group Restriction ─────────────────────
//
// A process may call sys_pledge(mask) to restrict itself to a subset of
// syscall groups.  The new mask is AND-ed into the existing mask — rights
// can only be removed, never added.  Once pledged, the restriction survives
// both fork (child inherits) and exec (new image starts with the same mask).
// This lets a parent set restrictions before exec that the child cannot escape.
//
// Enforcement: syscall_dispatch() checks pledge_mask BEFORE calling any
// handler.  Violation → SIGKILL delivered synchronously, handler never runs.
//
// Syscall groups (bit positions):
#define PLEDGE_STDIO      (1u <<  0)  // read/write/close/fstat/poll on open fds
#define PLEDGE_RPATH      (1u <<  1)  // sys_open O_RDONLY, stat, access, readdir
#define PLEDGE_WPATH      (1u <<  2)  // sys_open O_WRONLY / O_RDWR
#define PLEDGE_CPATH      (1u <<  3)  // creat, mkdir, unlink, rename, symlink, link
#define PLEDGE_EXEC       (1u <<  4)  // execve, spawn
#define PLEDGE_PROC       (1u <<  5)  // fork, waitpid, kill, getpid, getppid, setpgid
#define PLEDGE_INET       (1u <<  6)  // socket(AF_INET), connect, sendto, recvfrom
#define PLEDGE_UNIX       (1u <<  7)  // socket(AF_UNIX), bind, accept
#define PLEDGE_SIGNAL     (1u <<  8)  // sigaction, sigprocmask, sigreturn
#define PLEDGE_THREAD     (1u <<  9)  // sys_thread
#define PLEDGE_PROT_EXEC  (1u << 10)  // mmap(PROT_EXEC), mprotect(PROT_EXEC)
#define PLEDGE_SETUID     (1u << 11)  // setuid, seteuid, setgid, setegid, setgroups
#define PLEDGE_CHOWN      (1u << 12)  // chown, fchown
#define PLEDGE_CHMOD      (1u << 13)  // chmod, fchmod
#define PLEDGE_TTY        (1u << 14)  // ioctl on ttys, tcgetattr/tcsetattr
#define PLEDGE_IOCTL      (1u << 15)  // ioctl on non-tty fds
#define PLEDGE_SENDFD     (1u << 16)  // sys_sendfd, sys_recvfd
#define PLEDGE_KSEC       (1u << 17)  // sys_register_policy_agent (boot-only, uid=0)
#define PLEDGE_SHM        (1u << 18)  // shm_open, shm_unlink
#define PLEDGE_FRAMEBUF   (1u << 19)  // SYS_FB_MAP, SYS_FB_BLIT, SYS_FB_INFO

// Default mask: all groups permitted (unpledged process).
#define PLEDGE_ALL        (~0u)

// Reduce mask: AND-only, returns the new (more restricted) mask.
static inline uint32_t pledge_restrict(uint32_t current, uint32_t mask) {
    return current & mask;
}

// Check if syscall group `group` (single bit) is permitted.
static inline int pledge_allowed(uint32_t mask, uint32_t group) {
    return (mask & group) != 0;
}

// Map a syscall number to the pledge group it belongs to.
// Returns 0 if the syscall is always allowed (e.g. getpid, clock_ns).
// Returns PLEDGE_* if it needs the corresponding group.
// Defined in pledge.c.
uint32_t pledge_group_for_syscall(uint64_t nr);
