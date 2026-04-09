#pragma once
#include "../common.h"
#include "../vfs.h"
#include "skbuff.h"

// ── BSD Socket abstraction ────────────────────────────────────────────────
// Wraps TCP and UDP endpoints behind the kernel VFS interface so that
// user-space can use them via ordinary read()/write() file descriptors.
//
// Socket syscalls (added to syscall.h / syscall.c):
//   SYS_SOCKET   — create a socket fd
//   SYS_BIND     — bind to a local port
//   SYS_LISTEN   — mark as passive (server)
//   SYS_ACCEPT   — accept a new connection → new fd
//   SYS_CONNECT  — initiate active connection
//   SYS_SENDTO   — send data (or dgram to addr)
//   SYS_RECVFROM — receive data (and source address for UDP)
//   SYS_SETSOCKOPT — set socket options (stub)
//   SYS_SHUTDOWN — half-close a TCP socket

// ── Address family / socket type constants ────────────────────────────────
// Kept POSIX-compatible.
#define AF_INET     2
#define SOCK_STREAM 1   // TCP
#define SOCK_DGRAM  2   // UDP

// ── sockaddr_in (IPv4) ────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t sin_family;  // AF_INET
    uint16_t sin_port;    // network byte order
    uint32_t sin_addr;    // network byte order
    uint8_t  sin_zero[8]; // padding (unused)
} sockaddr_in_t;

// ── Socket shutdown flags ─────────────────────────────────────────────────
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// ── Internal socket state ─────────────────────────────────────────────────
// socket_t is the kernel-side representation; it is reference-counted via
// vfs_file_t.ctx.

struct tcp_pcb;  // forward declaration from tcp.h

typedef struct socket_t {
    uint8_t  type;         // SOCK_STREAM or SOCK_DGRAM
    uint8_t  bound;        // 1 if bind() has been called
    uint16_t local_port;   // host byte order

    // TCP-specific.
    struct tcp_pcb* pcb;   // NULL for UDP

    // UDP receive queue: a simple singly-linked list of skbuffs.
    // Protected by the single-threaded kernel (no SMP yet).
    skbuff_t* udp_rx_head;
    skbuff_t* udp_rx_tail;
    uint16_t  udp_rx_count;
    uint32_t  udp_peer_ip;   // last sender (network byte order)
    uint16_t  udp_peer_port; // last sender port (host byte order)

    // Task sleeping in socket_recv / socket_recvfrom (UDP) or
    // socket_accept / socket_connect (TCP).
    void* waiter;   // task_t* — avoid circular include with process.h
} socket_t;

// ── Kernel-internal API ───────────────────────────────────────────────────

// Create a socket and wrap it in a vfs_file_t.
// `domain` must be AF_INET.
// `type`   must be SOCK_STREAM or SOCK_DGRAM.
// Returns a heap-allocated vfs_file_t (refcount=1), or NULL on failure.
vfs_file_t* socket_open(int domain, int type);

// Bind a socket to a local port (host byte order).
// Returns 0 on success, -1 on error.
int socket_bind(vfs_file_t* f, uint16_t port);

// TCP-only: mark as listening for incoming connections.
// Returns 0 on success, -1 on error.
int socket_listen(vfs_file_t* f);

// TCP-only: accept a new connection from a listening socket.
// Blocks until a connection is ready (calls sched_sleep internally).
// Returns a new vfs_file_t (refcount=1) wrapping the accepted TCP PCB,
// and fills *peer_addr with the remote address (may be NULL).
// Returns NULL on error.
vfs_file_t* socket_accept(vfs_file_t* f, sockaddr_in_t* peer_addr);

// Initiate an active TCP connection to (dst_ip_be, dst_port host-order).
// Blocks until the connection is established or fails.
// Returns 0 on success, -1 on error.
int socket_connect(vfs_file_t* f, uint32_t dst_ip_be, uint16_t dst_port);

// Send data on a connected socket.
// For TCP: copies `len` bytes from `buf`, blocks if the send window is full.
// For UDP: sends a datagram to the last connect()-ed peer.
// Returns bytes sent, or -1 on error.
int socket_send(vfs_file_t* f, const void* buf, uint32_t len);

// Receive data from a connected socket.
// For TCP: fills buf with up to `len` bytes, blocks if none available.
// For UDP: fills buf with one datagram (up to `len` bytes).
// Returns bytes received, 0 on graceful close, -1 on error.
int socket_recv(vfs_file_t* f, void* buf, uint32_t len);

// Send a UDP datagram to (dst_ip_be, dst_port host-order).
int socket_sendto(vfs_file_t* f, const void* buf, uint32_t len,
                   uint32_t dst_ip_be, uint16_t dst_port);

// Receive a UDP datagram and fill in the source address.
int socket_recvfrom(vfs_file_t* f, void* buf, uint32_t len,
                     sockaddr_in_t* src_addr);

// Half-close: shutdown(how) — SHUT_WR sends TCP FIN.
int socket_shutdown(vfs_file_t* f, int how);

// ── Called by the UDP layer to deliver an incoming datagram ──────────────
// `dst_port` is in host byte order (already converted by udp_recv).
void socket_deliver_udp(uint16_t dst_port, skbuff_t* skb);
