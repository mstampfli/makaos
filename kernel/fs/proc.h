#pragma once
#include "common.h"
#include "vfs.h"
#include "ext2.h"
#include "syscall.h"

// ── /proc virtual filesystem ──────────────────────────────────────────────
//
// /proc is a read-only virtual filesystem that exposes kernel state as files.
// No data lives on disk — every read synthesizes content live from kernel
// memory.  The interface mirrors Linux /proc closely so userland tools work
// with minimal porting effort.
//
// Supported paths:
//
//   /proc/                       → directory: one entry per live PID + "self"
//   /proc/self                   → symlink to /proc/<current-pid>
//   /proc/<pid>/                 → directory for process <pid>
//   /proc/<pid>/status           → human-readable process info
//   /proc/<pid>/cmdline          → argv[0]\0 (NUL-terminated process name)
//   /proc/<pid>/fd/              → directory: one entry per open fd
//   /proc/<pid>/fd/<n>           → open returns dup of fd n
//   /proc/<pid>/maps             → VMA list (address range, perms, name)
//
// Extension points:
//   Add new per-process files by adding entries to s_proc_pid_files[] in proc.c.
//   Add new top-level files by adding entries to s_proc_root_files[] in proc.c.
//   Each entry is a { path_suffix, open_fn } pair — no other code changes needed.

// ── Public API ────────────────────────────────────────────────────────────

// Open a /proc path.  Called from sys_open when path starts with "/proc/".
// Returns heap-allocated vfs_file_t* (caller must vfs_close), or NULL on error.
vfs_file_t* proc_open(const char* path);

// List a /proc directory.  Called from sys_readdir when path starts with "/proc".
// Fills `out` with up to `max` entries.  Returns count, or -1 on error.
int proc_readdir(const char* path, ext2_entry_t* out, int max);

// stat a /proc path.  Called from sys_stat when path starts with "/proc".
// Returns 0 on success, -1 on error.
int proc_stat(const char* path, struct stat* out);
