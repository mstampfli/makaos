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

// Master-side read ring buffer.  Must be a power of two for the
// `(i + 1) & (PTY_MASTER_BUF - 1)` mask.  Allocated out-of-line so the
// pty_t struct itself stays small (fits in a slab cache) and the ring
// size can be tuned without changing sizeof(pty_t).
//
// 64 KiB is big enough to absorb a typical `ps` / `ls -la` / `cat
// biglog.txt` burst between terminal reader drains.  When full, the
// slave writer blocks on `slave_drain_waitq` and is woken whenever the
// master side drains bytes — classic flow control, no drops.
#define PTY_MASTER_BUF 65536u

typedef struct pty {
    tty_t        slave;                        // full tty with line discipline
    uint8_t*     master_buf;                    // ring: slave output → master read (kmalloc'd)
    uint32_t     m_head;                        // master ring head (write position)
    uint32_t     m_tail;                        // master ring tail (read position)
    struct vfs_file_t* master_file;             // back-pointer for master poll wakeups
    wait_queue_t master_waitq;                  // wait queue for master-side
                                                // blocking reads + poll/epoll
    wait_queue_t slave_drain_waitq;             // wait queue for slave-side writers
                                                // blocked on a full ring — woken by
                                                // pty_master_read after it drains
    int          master_open;                   // master fd still open?
    int          slave_open_count;              // refcount of open slave fds
    int          index;                         // pty number (for /dev/pts/N)
    // The slave vfs_file created by pty_alloc is PARKED here when the
    // pair comes from a /dev/ptmx open (posix_openpt model): the
    // caller only receives the master and claims the slave later via
    // /dev/pts/<index>.  slave_claimed flips on the first such open;
    // further opens share the same file with a refcount bump.
    struct vfs_file_t* slave_file;
    int          slave_claimed;
    struct pty*  next;                          // next node in live PTY list
} pty_t;

// Allocate a new PTY pair. Returns master and slave vfs_file_t pointers.
// Caller installs them into the fd table.
// Returns 0 on success, -errno on failure.
int pty_alloc(vfs_file_t** master_out, vfs_file_t** slave_out);

// /dev/ptmx open: allocate a pair, park the slave handle on the pty
// (claimed later through /dev/pts/<index>), return the master.
vfs_file_t* pty_open_master(void);

// /dev/pts/<n> open: claim (or share) the parked slave handle of the
// live pty with that index.  NULL if no such pty.
vfs_file_t* pty_open_slave_by_index(int n);

// Called by slave's write_char to push output to master's read buffer.
void pty_master_push(pty_t* pty, uint8_t c);

// Head of the singly-linked list of live (not-yet-fully-closed) PTYs.
// Used by tty_get_ctty to find PTY slaves by session.
pty_t* pty_list_head(void);
