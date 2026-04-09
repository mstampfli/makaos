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
// Copies `len` bytes from `data` into the send buffer.
// Blocks if the send window is full.
int tcp_send(tcp_pcb_t* pcb, const void* data, uint32_t len);

// Receive data from an established connection.
// Copies up to `len` bytes into `buf`.
// Blocks if no data is available and the connection is still open.
// Returns 0 on graceful close, -1 on error.
int tcp_recv_data(tcp_pcb_t* pcb, void* buf, uint32_t len);

// Initiate graceful close (send FIN).
void tcp_close(tcp_pcb_t* pcb);

// Query the state of a PCB.
tcp_state_t tcp_pcb_state(const tcp_pcb_t* pcb);

// Current local port assignment (for connect without explicit bind).
uint16_t tcp_ephemeral_port(void);

// Set/clear the waiter task pointer so the socket layer can sleep on a PCB.
// `waiter` is a task_t*; declared void* to avoid a circular dependency with
// process.h.  The TCP layer calls sched_wake(pcb->waiter) at state changes.
void tcp_pcb_set_waiter(tcp_pcb_t* pcb, void* waiter);
