#include "pledge.h"
#include "syscall.h"

// ── Syscall → pledge group mapping ───────────────────────────────────────
//
// Returns the single PLEDGE_* bit that governs this syscall number.
// Returns 0 for syscalls that are unconditionally permitted regardless of
// pledge (e.g. getpid, clock_ns, sigreturn — these are either informational
// or required for correct signal handling and cannot be reasonably restricted).

uint32_t pledge_group_for_syscall(uint64_t nr) {
    switch ((uint32_t)nr) {

    // ── Always permitted (no pledge group) ────────────────────────────────
    case SYS_GETPID:
    case SYS_GETPPID:
    case SYS_CLOCK_NS:
    case SYS_GETTOD:
    case SYS_SIGRETURN:   // must always be allowed for signal unwinding
    case SYS_GETPGRP:
    case SYS_GETSID:
    case SYS_GETPGID:
        return 0;

    // ── PLEDGE_STDIO ──────────────────────────────────────────────────────
    case SYS_READ:
    case SYS_WRITE:
    case SYS_CLOSE:
    case SYS_FSTAT:
    case SYS_LSEEK:
    case SYS_DUP:
    case SYS_DUP2:
    case SYS_FCNTL:
    case SYS_POLL:
    case SYS_SELECT:
    case SYS_NANOSLEEP:
    case SYS_UNAME:
    case SYS_UMASK:
    case SYS_GETUID:
    case SYS_GETEUID:
    case SYS_GETGID:
    case SYS_GETEGID:
    case SYS_GETGROUPS:
    case SYS_TIMES:
    case SYS_GETRUSAGE:
        return PLEDGE_STDIO;

    // ── PLEDGE_RPATH ──────────────────────────────────────────────────────
    case SYS_STAT:
    case SYS_ACCESS:
    case SYS_READDIR:
    case SYS_READLINK:
    case SYS_GETCWD:
        return PLEDGE_RPATH;

    // ── PLEDGE_WPATH ──────────────────────────────────────────────────────
    // SYS_OPEN is checked in the handler (access mode determines group).
    // Listed here for completeness — handler performs finer-grained check.
    case SYS_OPEN:
        return 0;   // handler checks PLEDGE_RPATH / PLEDGE_WPATH / PLEDGE_CPATH

    // ── PLEDGE_CPATH ──────────────────────────────────────────────────────
    case SYS_MKDIR:
    case SYS_UNLINK:
    case SYS_RENAME:
    case SYS_SYMLINK:
    case SYS_LINK:
    case SYS_TRUNCATE:
    case SYS_FTRUNCATE:
        return PLEDGE_CPATH;

    // ── PLEDGE_EXEC ───────────────────────────────────────────────────────
    case SYS_EXEC:
    case SYS_SPAWN:
        return PLEDGE_EXEC;

    // ── PLEDGE_PROC ───────────────────────────────────────────────────────
    case SYS_FORK:
    case SYS_WAIT:
    case SYS_KILL:
    case SYS_SETPGID:
    case SYS_SETSID:
    case SYS_TCGETPGRP:
    case SYS_TCSETPGRP:
        return PLEDGE_PROC;

    // ── PLEDGE_INET ───────────────────────────────────────────────────────
    case SYS_SOCKET:
    case SYS_BIND:
    case SYS_LISTEN:
    case SYS_ACCEPT:
    case SYS_CONNECT:
    case SYS_SENDTO:
    case SYS_RECVFROM:
    case SYS_SETSOCKOPT:
    case SYS_SHUTDOWN:
        return PLEDGE_INET;   // UNIX sockets further restricted in handler

    // ── PLEDGE_SIGNAL ─────────────────────────────────────────────────────
    case SYS_SIGACTION:
    case SYS_SIGPROCMASK:
        return PLEDGE_SIGNAL;

    // ── PLEDGE_THREAD ─────────────────────────────────────────────────────
    case SYS_THREAD:
        return PLEDGE_THREAD;

    // ── PLEDGE_PROT_EXEC ─────────────────────────────────────────────────
    // mmap is checked in the handler — PROT_EXEC triggers this group.
    case SYS_MMAP:
    case SYS_MUNMAP:
        return 0;   // handler checks PLEDGE_PROT_EXEC for PROT_EXEC maps

    // ── PLEDGE_SETUID ─────────────────────────────────────────────────────
    // setuid/setgid syscalls — handled via new SYS_SETUID etc.
    // (numbers assigned in syscall.h additions)

    // ── PLEDGE_CHMOD / PLEDGE_CHOWN ───────────────────────────────────────
    case SYS_CHMOD:
    case SYS_FCHMOD:
        return PLEDGE_CHMOD;
    case SYS_CHOWN:
    case SYS_FCHOWN:
        return PLEDGE_CHOWN;

    // ── PLEDGE_TTY ────────────────────────────────────────────────────────
    case SYS_IOCTL:
        return 0;   // handler distinguishes tty vs non-tty for pledge

    // ── PLEDGE_SENDFD ─────────────────────────────────────────────────────
    // SYS_SENDFD / SYS_RECVFD / SYS_RESTRICT_FD handled in handler.

    // ── PLEDGE_KSEC ───────────────────────────────────────────────────────
    // SYS_REGISTER_POLICY_AGENT handled in handler.

    // ── PLEDGE_SHM ────────────────────────────────────────────────────────
    case SYS_SHM_OPEN:
    case SYS_SHM_UNLINK:
        return PLEDGE_SHM;

    // ── FB / graphics — no pledge group yet (always allowed if fd open) ──
    case SYS_FB_BLIT:
    case SYS_FB_INFO:
        return PLEDGE_STDIO;

    case SYS_CHDIR:
        return PLEDGE_RPATH;

    case SYS_REBOOT:
        return 0;   // checked against euid==0 in handler

    default:
        // Unknown syscall: allow dispatch, handler returns -ENOSYS.
        return 0;
    }
}
