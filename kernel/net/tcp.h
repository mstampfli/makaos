#pragma once
#include "common.h"
#include "skbuff.h"

// ── TCP (RFC 9293) ────────────────────────────────────────────────────────
// Full connection-oriented reliable transport with:
//   • Three-way handshake (SYN, SYN-ACK, ACK)
//   • Sliding window flow control
//   • Retransmission timer (simple timeout-based, no Karn's algorithm yet)
//   • Connection teardown (FIN/FIN-ACK/ACK)
//   • RST handling

#define TCP_HDR_MIN_LEN  20u   // minimum TCP header (no options)

// TCP flags
#define TCP_FIN  (1u << 0)
#define TCP_SYN  (1u << 1)
#define TCP_RST  (1u << 2)
#define TCP_PSH  (1u << 3)
#define TCP_ACK  (1u << 4)
#define TCP_URG  (1u << 5)

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   // [7:4] = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

// TCP connection state machine (RFC 9293 §3.3.2).
typedef enum {
    TCP_CLOSED      = 0,
    TCP_LISTEN      = 1,
    TCP_SYN_SENT    = 2,
    TCP_SYN_RCVD    = 3,
    TCP_ESTABLISHED = 4,
    TCP_FIN_WAIT_1  = 5,
    TCP_FIN_WAIT_2  = 6,
    TCP_CLOSE_WAIT  = 7,
    TCP_CLOSING     = 8,
    TCP_LAST_ACK    = 9,
    TCP_TIME_WAIT   = 10,
} tcp_state_t;

// Receive a TCP segment (called by ipv4_recv).
void tcp_recv(skbuff_t* skb);

// Called periodically by the net thread to retransmit unacknowledged data
// and advance TIME_WAIT timers.
void tcp_timer_tick(void);

// ── Internal TCP control block (TCB) — defined in tcp.c ──────────────────
// tcp.h exposes the handle as a forward declaration; socket.c uses it.
struct tcp_pcb;
typedef struct tcp_pcb tcp_pcb_t;

// Allocate a new TCB bound to local port `lport`.
tcp_pcb_t* tcp_pcb_alloc(uint16_t lport);

// Release a TCB (called when the socket is closed).
void tcp_pcb_free(tcp_pcb_t* pcb);

// Initiate an active open (connect).
// Returns 0 if the SYN has been sent; the caller must poll tcp_pcb_state().
int tcp_connect(tcp_pcb_t* pcb, uint32_t dst_ip_be, uint16_t dst_port);

// Put the PCB into the LISTEN state (passive open).
int tcp_listen(tcp_pcb_t* pcb);

// Accept the first completed connection from a listening PCB.
// Non-blocking: returns NULL if no connection is ready.
tcp_pcb_t* tcp_accept(tcp_pcb_t* listener);

// Send data on an established connection.
// Copies up to `len` bytes from `data` into the send buffer.
// If `nonblock` is 0, blocks while the send buffer is full.
// If `nonblock` is non-zero, returns -EAGAIN when no progress can be made.
// Returns the number of bytes queued on success, or a negative errno on error:
//   -EPIPE        — connection is CLOSED or peer has closed its receive side
//   -ENOTCONN     — PCB is not in a state that accepts outbound data
//   -EAGAIN       — nonblock mode and buffer full
int tcp_send(tcp_pcb_t* pcb, const void* data, uint32_t len, int nonblock);

// Receive data from an established connection.
// Copies up to `len` bytes into `buf`. Returns the number of bytes read.
// Returns 0 on graceful EOF (peer FIN + drained rx buffer).
// Returns a negative errno on error:
//   -EAGAIN       — nonblock mode, nothing available
//   -ENOTCONN     — PCB never connected / was reset
//   -ECONNRESET   — connection was reset while reading
int tcp_recv_data(tcp_pcb_t* pcb, void* buf, uint32_t len, int nonblock);

// Initiate graceful close (send FIN).
void tcp_close(tcp_pcb_t* pcb);

// Query the state of a PCB.
tcp_state_t tcp_pcb_state(const tcp_pcb_t* pcb);

// Non-blocking readiness / state queries used by the socket poll handler.
// tcp_pcb_rx_used   — bytes of receive data immediately available.
// tcp_pcb_tx_space  — free bytes in send buffer.
// tcp_pcb_eof       — 1 if peer FIN has been seen (recv will return 0).
// tcp_pcb_has_accept — 1 if a listener has a completed child waiting in its
//                      accept queue.
uint32_t tcp_pcb_rx_used  (const tcp_pcb_t* pcb);
uint32_t tcp_pcb_tx_space (const tcp_pcb_t* pcb);
int      tcp_pcb_eof      (const tcp_pcb_t* pcb);
int      tcp_pcb_has_accept(const tcp_pcb_t* pcb);

// Current local port assignment (for connect without explicit bind).
uint16_t tcp_ephemeral_port(void);

// Set/clear the waiter task pointer so the socket layer can sleep on a PCB.
// `waiter` is a task_t*; declared void* to avoid a circular dependency with
// process.h.  The TCP layer calls sched_wake(pcb->waiter) at state changes.
void tcp_pcb_set_waiter(tcp_pcb_t* pcb, void* waiter);

// Bind a vfs_file_t* backpointer to this PCB so that pcb_wake() can also
// wake any task sleeping in poll()/select() on the socket fd.  Declared as
// void* to avoid dragging vfs.h into tcp.h.
void tcp_pcb_set_file(tcp_pcb_t* pcb, void* file);

// Rebind an ephemeral, CLOSED pcb's local port in place (socket_bind) -- no
// free/realloc, so a concurrent socket op derefing s->pcb cannot hit a freed pcb.
void tcp_pcb_set_local_port(tcp_pcb_t* pcb, uint16_t port);
