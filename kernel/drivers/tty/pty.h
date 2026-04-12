#pragma once
#include "tty.h"
#include "vfs.h"

// ── Pseudo-terminal (PTY) subsystem ─────────────────────────────────────
//
// A PTY is a bidirectional channel between a master fd and a slave tty_t:
//
//   terminal emulator              shell / child process
//        |                                 |
//   master fd write  ──→  tty_input_char(slave)  ──→  slave read()
//   master fd read   ←──  slave write_char()      ←──  slave write()
//
// The slave side is a full tty_t with line discipline (echo, ^C, canonical
// mode, etc.) — programs using the slave cannot tell it apart from a real
// hardware terminal.
//
// The master side is a plain fd: write injects input into the slave,
// read receives the slave's output.

#define PTY_MAX        16   // maximum simultaneous PTY pairs
#define PTY_MASTER_BUF 4096 // master-side read ring buffer size (power of 2)

typedef struct pty {
    tty_t        slave;                        // full tty with line discipline
    uint8_t      master_buf[PTY_MASTER_BUF];   // ring: slave output → master read
    uint32_t     m_head;                        // master ring head (write position)
    uint32_t     m_tail;                        // master ring tail (read position)
    struct task_t* m_reader;                    // task sleeping in master read()
    struct vfs_file_t* master_file;             // back-pointer for poll wakeups
    int          allocated;                     // 1 = in use
    int          master_open;                   // master fd still open?
    int          slave_open_count;              // refcount of open slave fds
    int          index;                         // pty number (for /dev/pts/N)
} pty_t;

// Allocate a new PTY pair. Returns master and slave vfs_file_t pointers.
// Caller installs them into the fd table.
// Returns 0 on success, -errno on failure.
int pty_alloc(vfs_file_t** master_out, vfs_file_t** slave_out);

// Called by slave's write_char to push output to master's read buffer.
void pty_master_push(pty_t* pty, uint8_t c);

// Global PTY table (for tty_get_ctty lookup)
extern pty_t g_ptys[PTY_MAX];
