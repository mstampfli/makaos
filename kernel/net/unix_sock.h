#pragma once
#include "common.h"
#include "vfs.h"
#include "net/socket.h"  // AF_UNIX, SOCK_STREAM, SOCK_DGRAM

// ── Unix Domain Sockets (AF_UNIX) ───────────────────────────────────────
//
// Local IPC mechanism: bidirectional byte-stream (SOCK_STREAM) or
// datagram (SOCK_DGRAM) communication between processes on the same host.
//
// Key differences from AF_INET sockets:
//   - Addressed by filesystem path (sockaddr_un), not IP:port.
//   - No network stack involved — data goes through kernel buffers only.
//   - Supports SCM_RIGHTS: passing file descriptors between processes.
//   - SOCK_STREAM is connection-oriented (listen/accept/connect).
//   - SOCK_DGRAM is connectionless but reliable (no packet loss).
//
// Architecture:
//   - unix_sock_t is the kernel-side state (embedded in vfs_file_t->ctx).
//   - SOCK_STREAM pairs: connect() links two unix_sock_t via peer pointers.
//     Data written to one appears in the other's receive buffer.
//   - SOCK_DGRAM: sendto() delivers directly to the bound target socket's
//     receive queue.  No connection required.
//   - Bind namespace: flat kernel table mapping path → unix_sock_t*.
//   - Accept backlog: singly-linked list of pending connections on listener.

// ── Constants ────────────────────────────────────────────────────────────

// AF_UNIX, AF_LOCAL, SOCK_STREAM, SOCK_DGRAM are in socket.h.

#define UNIX_PATH_MAX   108    // POSIX sockaddr_un path length

// Internal buffer size for SOCK_STREAM data and SOCK_DGRAM messages.
#define UNIX_BUF_SIZE   8192   // 8 KiB circular buffer per socket

// Maximum pending connections on a listening socket.
#define UNIX_BACKLOG_MAX 16

// Maximum bound unix sockets in the namespace (dynamic — no fixed cap).
// UNIX_NS_MAX kept for ABI compatibility; dynamic hash table used internally.
#define UNIX_NS_MAX      64  // unused — table grows dynamically

// Maximum in-flight fds in the SCM_RIGHTS ancillary queue.
#define UNIX_ANCILLARY_MAX 8

// ── sockaddr_un ──────────────────────────────────────────────────────────

typedef struct {
    uint16_t sun_family;               // AF_UNIX
    char     sun_path[UNIX_PATH_MAX];  // null-terminated path
} sockaddr_un_t;

// ── Connection states ────────────────────────────────────────────────────

enum {
    UNIX_STATE_UNCONNECTED = 0,  // freshly created
    UNIX_STATE_BOUND,            // bind() called
    UNIX_STATE_LISTENING,        // listen() called (SOCK_STREAM only)
    UNIX_STATE_CONNECTING,       // connect() in progress
    UNIX_STATE_CONNECTED,        // connected (paired with peer)
    UNIX_STATE_DISCONNECTED,     // peer closed its end
};

// ── Datagram message ─────────────────────────────────────────────────────
// For SOCK_DGRAM, each message is a discrete unit with sender info.

typedef struct unix_dgram {
    struct unix_dgram* next;
    uint32_t           len;
    // Variable-length data follows this header (allocated as one block).
    // Access via UNIX_DGRAM_DATA(msg).
} unix_dgram_t;

#define UNIX_DGRAM_DATA(msg) ((uint8_t*)((msg) + 1))

// ── Pending connection (accept backlog) ──────────────────────────────────

typedef struct unix_pending {
    struct unix_pending* next;
    struct unix_sock*    client;  // the connecting socket
} unix_pending_t;

// ── SCM_RIGHTS ancillary data ────────────────────────────────────────────
// In-flight file descriptors waiting to be received via recvfd().

typedef struct {
    vfs_file_t* files[UNIX_ANCILLARY_MAX];
    uint32_t    rights[UNIX_ANCILLARY_MAX]; // attenuated rights mask per fd
    uint8_t     head;
    uint8_t     tail;
    uint8_t     count;
} unix_ancillary_t;

// ── Unix socket kernel object ────────────────────────────────────────────

typedef struct unix_sock {
    uint8_t  type;          // SOCK_STREAM or SOCK_DGRAM
    uint8_t  state;         // UNIX_STATE_*
    uint8_t  shutdown_rd;   // 1 if read half shut down
    uint8_t  shutdown_wr;   // 1 if write half shut down

    // Bind path (empty = unbound).  Used for namespace lookup.
    char     path[UNIX_PATH_MAX];

    // ── SOCK_STREAM data buffer (circular) ───────────────────────────────
    // Written by peer, read by owner.  Protected by single-threaded kernel.
    uint8_t  buf[UNIX_BUF_SIZE];
    uint32_t buf_head;      // read index
    uint32_t buf_tail;      // write index
    uint32_t buf_count;     // bytes in buffer

    // ── SOCK_DGRAM receive queue ─────────────────────────────────────────
    unix_dgram_t* dgram_head;
    unix_dgram_t* dgram_tail;
    uint32_t      dgram_count;

    // ── Peer (SOCK_STREAM connected pair) ────────────────────────────────
    struct unix_sock* peer;

    // ── Listener backlog (SOCK_STREAM only) ──────────────────────────────
    unix_pending_t* backlog_head;
    unix_pending_t* backlog_tail;
    uint32_t        backlog_count;
    uint32_t        backlog_max;

    // ── SCM_RIGHTS ancillary queue ───────────────────────────────────────
    unix_ancillary_t ancillary;

    // ── Blocking support ─────────────────────────────────────────────────
    // Wait queue for tasks sleeping on this socket (accept, recv,
    // connect, send-when-full).  SMP-safe lock-free MPSC queue.
    wait_queue_t waitq;

    // Back-pointer to the vfs_file_t wrapping this socket (for poll wakeups).
    struct vfs_file_t* file;

    // PID of the process on the OTHER end of this connection, as seen by the
    // kernel at accept()/connect() time. Trusted — never client-reported.
    // Used by the compositor to SIGKILL an unresponsive client after the user
    // force-closes its window. 0 = no peer (unconnected / listener).
    uint32_t peer_pid;
} unix_sock_t;

// ── Kernel API ───────────────────────────────────────────────────────────

// Create a unix socket and wrap in vfs_file_t.  type = SOCK_STREAM or SOCK_DGRAM.
vfs_file_t* unix_sock_open(int type);

// Bind to a filesystem path.  Returns 0 or -errno.
int unix_sock_bind(vfs_file_t* f, const char* path);

// Mark as listening (SOCK_STREAM only).  Returns 0 or -errno.
int unix_sock_listen(vfs_file_t* f, int backlog);

// Accept a pending connection (blocks).  Returns new vfs_file_t or NULL.
vfs_file_t* unix_sock_accept(vfs_file_t* f);

// Connect to a bound path (SOCK_STREAM: blocks until accepted; SOCK_DGRAM: sets default peer).
int unix_sock_connect(vfs_file_t* f, const char* path);

// Send data.  SOCK_STREAM: to peer.  SOCK_DGRAM: to connected peer or last sendto target.
int unix_sock_send(vfs_file_t* f, const void* buf, uint32_t len);

// Receive data.  Blocks if empty.  Returns bytes read, 0 on EOF, -errno on error.
int unix_sock_recv(vfs_file_t* f, void* buf, uint32_t len);

// Send datagram to a specific path (SOCK_DGRAM only).
int unix_sock_sendto(vfs_file_t* f, const void* buf, uint32_t len, const char* path);

// Shutdown half or both directions.
int unix_sock_shutdown(vfs_file_t* f, int how);

// ── SCM_RIGHTS ───────────────────────────────────────────────────────────

// Queue a file descriptor for delivery to the peer (called by sys_sendfd).
// `file` is dup'd; `rights` is the attenuated rights mask.
int unix_sock_sendfd(vfs_file_t* sock, vfs_file_t* file, uint32_t rights);

// Dequeue a received file descriptor (called by sys_recvfd).
// Returns a new vfs_file_t with stamped rights, or NULL if queue empty.
vfs_file_t* unix_sock_recvfd(vfs_file_t* sock);

// ── VFS close callback (exported for type identification) ────────────────
void unix_sock_close(vfs_file_t* self);
