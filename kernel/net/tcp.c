#include "tcp.h"
#include "ipv4.h"
#include "eth.h"
#include "net.h"
#include "socket.h"
#include "common.h"
#include "kheap.h"
#include "sched.h"
#include "tsc.h"
#include "errno.h"
#include "vfs.h"
#include "syscall.h"  // POLLIN / POLLOUT / POLLHUP / POLLERR

// ── TCP constants ─────────────────────────────────────────────────────────
#define TCP_RTO_NS        1000000000ULL   // retransmit timeout: 1 second
#define TCP_MSS           1460u           // max segment size (1500 - 20 IP - 20 TCP)
#define TCP_WINDOW        65535u          // advertised receive window
#define TCP_TIME_WAIT_NS  4000000000ULL   // TIME_WAIT: 2×MSL = 4 seconds
#define TCP_MAX_PCBS      64u             // maximum simultaneous connections
#define TCP_BACKLOG       8u              // listen backlog per server socket
#define TCP_RXBUF_SIZE    65536u          // per-connection receive ring (power of 2)
#define TCP_TXBUF_SIZE    65536u          // per-connection transmit ring (power of 2)
#define TCP_RXBUF_MASK    (TCP_RXBUF_SIZE - 1u)
#define TCP_TXBUF_MASK    (TCP_TXBUF_SIZE - 1u)

// ── TCP control block ─────────────────────────────────────────────────────
struct tcp_pcb {
    tcp_state_t state;

    uint32_t local_ip;   // network byte order
    uint16_t local_port; // host byte order
    uint32_t remote_ip;  // network byte order
    uint16_t remote_port;// host byte order

    // Send sequence variables (RFC 9293 §3.3.1).
    uint32_t snd_una;    // oldest unacknowledged sequence number
    uint32_t snd_nxt;    // next sequence number to send
    uint32_t snd_wnd;    // send window (from receiver's advertisement)
    uint32_t iss;        // initial send sequence number

    // Receive sequence variables.
    uint32_t rcv_nxt;    // next expected receive sequence number
    uint32_t rcv_wnd;    // our advertised receive window
    uint32_t irs;        // initial receive sequence number

    // Retransmission.
    uint64_t rto_deadline;  // TSC nanoseconds when oldest unacked segment expires

    // Send buffer (ring).
    uint8_t* txbuf;
    uint32_t txbuf_head;   // read pointer (snd_una relative to start)
    uint32_t txbuf_tail;   // write pointer (consumer: TCP engine, producer: user)
    uint32_t txbuf_used;

    // Receive buffer (ring).
    uint8_t* rxbuf;
    uint32_t rxbuf_head;   // read pointer (consumer: user)
    uint32_t rxbuf_tail;   // write pointer (producer: TCP engine)
    uint32_t rxbuf_used;

    // For listening sockets: queue of completed connections.
    struct tcp_pcb* accept_queue[TCP_BACKLOG];
    uint8_t  accept_head;
    uint8_t  accept_tail;
    uint8_t  accept_count;

    // Backpointer to the listener that spawned this PCB (for SYN_RCVD).
    struct tcp_pcb* listener;

    // Waiter: the task sleeping on recv or connect.
    task_t* waiter;

    // Backpointer to the vfs_file_t wrapping this PCB on the socket layer.
    // Used by pcb_wake() to also wake any task sleeping in poll()/select()
    // on this socket's fd.  NULL for PCBs that do not have a socket fd yet
    // (e.g. SYN_RCVD children before accept()).  Stored as void* to avoid
    // dragging vfs.h into tcp.h.
    void*    sock_file;

    uint8_t  fin_sent;   // 1 if FIN has been queued for sending
    uint8_t  fin_rcvd;   // 1 if remote FIN received
    uint8_t  reset;      // 1 if the connection was aborted by RST
    uint64_t timewait_start; // when TIME_WAIT was entered
};

// ── PCB table ─────────────────────────────────────────────────────────────
static tcp_pcb_t s_pcbs[TCP_MAX_PCBS];
static uint8_t   s_pcb_used[TCP_MAX_PCBS];

// Ephemeral port counter.
static uint16_t s_eph_port = 49152u;

// ── Helpers ───────────────────────────────────────────────────────────────

static uint32_t seq32(void) {
    // Use TSC as a rough ISN source (not cryptographically secure,
    // but sufficient for a single-host OS).
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (hi << 16);
}

// Sequence number arithmetic — unsigned modular comparison.
#define SEQ_LT(a,b)  ((int32_t)((a)-(b)) < 0)
#define SEQ_LE(a,b)  ((int32_t)((a)-(b)) <= 0)
#define SEQ_GT(a,b)  ((int32_t)((a)-(b)) > 0)
#define SEQ_GE(a,b)  ((int32_t)((a)-(b)) >= 0)

// Wake any task blocked on this PCB: the blocking waiter (in recv/send/
// connect/accept) and any poll()/select() waiter camped on the socket fd.
// Both are fire-and-forget — a blocking task might be sleeping in recv
// while another one is polling the same fd (e.g. libdisplay-style event
// loops), and we must wake them both.
static void pcb_wake(tcp_pcb_t* pcb) {
    if (pcb->waiter) {
        task_t* t = pcb->waiter;
        pcb->waiter = NULL;
        sched_wake(t);
    }
    if (pcb->sock_file) {
        vfs_file_t* f = (vfs_file_t*)pcb->sock_file;
        wait_queue_wake_all(f->waitq);
    }
}

// ── Send a TCP segment ────────────────────────────────────────────────────

static int tcp_send_segment(tcp_pcb_t* pcb, uint8_t flags,
                              const void* data, uint16_t dlen) {
    uint16_t hdr_words = TCP_HDR_MIN_LEN / 4u;
    skbuff_t* skb = skb_alloc(TCP_HDR_MIN_LEN + dlen + IPV4_HDR_LEN);
    if (!skb) return -1;
    skb_reserve(skb, IPV4_HDR_LEN);

    tcp_hdr_t* h = (tcp_hdr_t*)skb_put(skb, TCP_HDR_MIN_LEN);
    if (!h) { skb_free(skb); return -1; }

    h->src_port = hton16(pcb->local_port);
    h->dst_port = hton16(pcb->remote_port);
    h->seq      = hton32(pcb->snd_nxt);
    h->ack      = (flags & TCP_ACK) ? hton32(pcb->rcv_nxt) : 0;
    h->data_off = (uint8_t)(hdr_words << 4);
    h->flags    = flags;
    h->window   = hton16((uint16_t)pcb->rcv_wnd);
    h->checksum = 0;
    h->urgent   = 0;

    if (dlen) {
        uint8_t* p = (uint8_t*)skb_put(skb, dlen);
        const uint8_t* src = (const uint8_t*)data;
        for (uint16_t i = 0; i < dlen; i++) p[i] = src[i];
    }

    // Compute TCP checksum.
    uint32_t pseudo = inet_pseudo_partial(pcb->local_ip, pcb->remote_ip,
                                           IPPROTO_TCP,
                                           (uint16_t)(TCP_HDR_MIN_LEN + dlen));
    const uint16_t* pw = (const uint16_t*)h;
    uint32_t sum = pseudo;
    uint16_t l = TCP_HDR_MIN_LEN + dlen;
    while (l > 1) { sum += *pw++; l -= 2; }
    if (l) sum += *(const uint8_t*)pw;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    h->checksum = (uint16_t)~sum;

    // Advance snd_nxt (SYN and FIN each consume one sequence number).
    if (flags & (TCP_SYN | TCP_FIN)) pcb->snd_nxt++;
    pcb->snd_nxt += dlen;

    // Arm retransmit timer if we sent unacknowledged data.
    if (SEQ_GT(pcb->snd_nxt, pcb->snd_una))
        pcb->rto_deadline = tsc_read_ns() + TCP_RTO_NS;

    int r = ipv4_send(skb, pcb->remote_ip, IPPROTO_TCP);
    skb_free(skb);
    return r;
}

// ── Send a RST ────────────────────────────────────────────────────────────

static void tcp_send_rst(uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port,
                          uint32_t seq, uint32_t ack_seq) {
    skbuff_t* skb = skb_alloc(TCP_HDR_MIN_LEN + IPV4_HDR_LEN);
    if (!skb) return;
    skb_reserve(skb, IPV4_HDR_LEN);

    tcp_hdr_t* h = (tcp_hdr_t*)skb_put(skb, TCP_HDR_MIN_LEN);
    if (!h) { skb_free(skb); return; }
    h->src_port = hton16(src_port);
    h->dst_port = hton16(dst_port);
    h->seq      = hton32(seq);
    h->ack      = hton32(ack_seq);
    h->data_off = (uint8_t)(5u << 4);
    h->flags    = TCP_RST | TCP_ACK;
    h->window   = 0;
    h->checksum = 0;
    h->urgent   = 0;

    uint32_t pseudo = inet_pseudo_partial(src_ip, dst_ip, IPPROTO_TCP,
                                           TCP_HDR_MIN_LEN);
    const uint16_t* pw = (const uint16_t*)h;
    uint32_t sum = pseudo;
    uint16_t l = TCP_HDR_MIN_LEN;
    while (l > 1) { sum += *pw++; l -= 2; }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    h->checksum = (uint16_t)~sum;

    ipv4_send(skb, dst_ip, IPPROTO_TCP);
    skb_free(skb);
}

// ── PCB lookup ────────────────────────────────────────────────────────────

static tcp_pcb_t* pcb_find(uint32_t local_ip, uint16_t local_port,
                             uint32_t remote_ip, uint16_t remote_port) {
    tcp_pcb_t* listener = NULL;
    for (uint32_t i = 0; i < TCP_MAX_PCBS; i++) {
        if (!s_pcb_used[i]) continue;
        tcp_pcb_t* p = &s_pcbs[i];
        // Exact match (ESTABLISHED / SYN_RCVD).
        if (p->local_port  == local_port  &&
            p->remote_port == remote_port &&
            p->remote_ip   == remote_ip   &&
            p->state       != TCP_LISTEN)
            return p;
        // Listener match.
        if (p->local_port == local_port && p->state == TCP_LISTEN)
            listener = p;
    }
    return listener;
}

// ── Receive ───────────────────────────────────────────────────────────────

void tcp_recv(skbuff_t* skb) {
    if (skb->len < TCP_HDR_MIN_LEN) { skb_free(skb); return; }

    tcp_hdr_t* seg = (tcp_hdr_t*)skb->data;
    uint8_t    hlen = (uint8_t)(((seg->data_off >> 4) & 0xFu) * 4u);
    if (hlen < TCP_HDR_MIN_LEN || hlen > skb->len) { skb_free(skb); return; }

    // Verify checksum.
    uint16_t saved = seg->checksum;
    seg->checksum  = 0;
    uint32_t pseudo = inet_pseudo_partial(skb->src_ip_be, skb->dst_ip_be,
                                           IPPROTO_TCP, (uint16_t)skb->len);
    const uint16_t* pw = (const uint16_t*)skb->data;
    uint32_t sum = pseudo;
    uint16_t l = (uint16_t)skb->len;
    while (l > 1) { sum += *pw++; l -= 2; }
    if (l) sum += *(const uint8_t*)pw;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t calc = (uint16_t)~sum;
    seg->checksum = saved;
    if (calc != saved) { skb_free(skb); return; }

    uint16_t src_port = ntoh16(seg->src_port);
    uint16_t dst_port = ntoh16(seg->dst_port);
    uint32_t seq      = ntoh32(seg->seq);
    uint32_t ack      = ntoh32(seg->ack);
    uint8_t  flags    = seg->flags;
    uint16_t dlen     = (uint16_t)(skb->len - hlen);
    uint8_t* data     = (uint8_t*)skb->data + hlen;
    uint32_t our_ip   = net_our_ip();

    tcp_pcb_t* pcb = pcb_find(our_ip, dst_port, skb->src_ip_be, src_port);

    if (!pcb) {
        // No PCB — send RST unless the incoming segment is already a RST.
        if (!(flags & TCP_RST))
            tcp_send_rst(our_ip, dst_port, skb->src_ip_be, src_port,
                         ack, seq + dlen + ((flags & TCP_SYN) ? 1u : 0u));
        skb_free(skb);
        return;
    }

    // ── State machine ────────────────────────────────────────────────────

    if (flags & TCP_RST) {
        if (pcb->state != TCP_LISTEN && pcb->state != TCP_CLOSED) {
            pcb->reset = 1;
            pcb->state = TCP_CLOSED;
            pcb_wake(pcb);
        }
        skb_free(skb);
        return;
    }

    switch (pcb->state) {

    case TCP_LISTEN:
        if (!(flags & TCP_SYN)) break;
        {
            // Spawn a new PCB for this incoming connection.
            tcp_pcb_t* child = tcp_pcb_alloc(dst_port);
            if (!child) {
                tcp_send_rst(our_ip, dst_port, skb->src_ip_be, src_port,
                             0, seq + 1u);
                break;
            }
            child->remote_ip   = skb->src_ip_be;
            child->remote_port = src_port;
            child->local_ip    = our_ip;
            child->irs         = seq;
            child->rcv_nxt     = seq + 1u;
            child->snd_wnd     = ntoh16(seg->window);
            child->listener    = pcb;
            child->state       = TCP_SYN_RCVD;
            child->iss         = seq32();
            child->snd_una     = child->iss;
            child->snd_nxt     = child->iss;

            // Send SYN-ACK.
            tcp_send_segment(child, TCP_SYN | TCP_ACK, NULL, 0);
        }
        break;

    case TCP_SYN_RCVD:
        if ((flags & TCP_ACK) && ntoh32(seg->ack) == pcb->snd_nxt) {
            pcb->snd_una = ntoh32(seg->ack);
            pcb->state   = TCP_ESTABLISHED;
            // Move to listener's accept queue.
            tcp_pcb_t* lst = pcb->listener;
            if (lst && lst->accept_count < TCP_BACKLOG) {
                lst->accept_queue[lst->accept_tail] = pcb;
                lst->accept_tail = (uint8_t)((lst->accept_tail + 1u) % TCP_BACKLOG);
                lst->accept_count++;
                pcb_wake(lst);
            }
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ntoh32(seg->ack) != pcb->snd_nxt) {
                tcp_send_rst(pcb->local_ip, pcb->local_port,
                             pcb->remote_ip, pcb->remote_port,
                             ntoh32(seg->ack), 0);
                break;
            }
            pcb->snd_una   = ntoh32(seg->ack);
            pcb->irs       = seq;
            pcb->rcv_nxt   = seq + 1u;
            pcb->snd_wnd   = ntoh16(seg->window);
            pcb->state     = TCP_ESTABLISHED;
            tcp_send_segment(pcb, TCP_ACK, NULL, 0);
            pcb_wake(pcb);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT: {
        // Process ACK — advance snd_una.
        if ((flags & TCP_ACK) && SEQ_GT(ack, pcb->snd_una) &&
            SEQ_LE(ack, pcb->snd_nxt)) {
            uint32_t acked = ack - pcb->snd_una;
            pcb->txbuf_head  = (pcb->txbuf_head + acked) & TCP_TXBUF_MASK;
            pcb->txbuf_used -= acked;
            pcb->snd_una     = ack;
            pcb->snd_wnd     = ntoh16(seg->window);
            if (pcb->snd_una == pcb->snd_nxt) pcb->rto_deadline = 0;
            pcb_wake(pcb);
        }

        // Receive in-order data.
        if (dlen > 0 && seq == pcb->rcv_nxt) {
            uint32_t space = TCP_RXBUF_SIZE - pcb->rxbuf_used;
            uint32_t copy  = dlen < space ? dlen : space;
            for (uint32_t i = 0; i < copy; i++)
                pcb->rxbuf[(pcb->rxbuf_tail + i) & TCP_RXBUF_MASK] = data[i];
            pcb->rxbuf_tail  = (pcb->rxbuf_tail + copy) & TCP_RXBUF_MASK;
            pcb->rxbuf_used += copy;
            pcb->rcv_nxt    += copy;
            pcb_wake(pcb);
            // Send ACK.
            tcp_send_segment(pcb, TCP_ACK, NULL, 0);
        }

        // FIN processing.
        if ((flags & TCP_FIN) && seq == pcb->rcv_nxt) {
            pcb->rcv_nxt++;
            pcb->fin_rcvd = 1;
            tcp_send_segment(pcb, TCP_ACK, NULL, 0);
            pcb_wake(pcb);

            if (pcb->state == TCP_ESTABLISHED) {
                pcb->state = TCP_CLOSE_WAIT;
            } else if (pcb->state == TCP_FIN_WAIT_1) {
                pcb->state = TCP_CLOSING;
            } else if (pcb->state == TCP_FIN_WAIT_2) {
                pcb->timewait_start = tsc_read_ns();
                pcb->state = TCP_TIME_WAIT;
            }
        }

        // Transition FIN_WAIT_1 → FIN_WAIT_2 when our FIN is acked.
        if (pcb->state == TCP_FIN_WAIT_1 && (flags & TCP_ACK) &&
            ack == pcb->snd_nxt)
            pcb->state = TCP_FIN_WAIT_2;

        // Transition CLOSING → TIME_WAIT.
        if (pcb->state == TCP_CLOSING && (flags & TCP_ACK) &&
            ack == pcb->snd_nxt) {
            pcb->timewait_start = tsc_read_ns();
            pcb->state = TCP_TIME_WAIT;
        }
        break;
    }

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack == pcb->snd_nxt) {
            pcb->state = TCP_CLOSED;
            pcb_wake(pcb);
        }
        break;

    default:
        break;
    }

    skb_free(skb);
}

// ── Timer tick ────────────────────────────────────────────────────────────

void tcp_timer_tick(void) {
    uint64_t now = tsc_read_ns();
    for (uint32_t i = 0; i < TCP_MAX_PCBS; i++) {
        if (!s_pcb_used[i]) continue;
        tcp_pcb_t* pcb = &s_pcbs[i];

        // Retransmit timeout.
        if (pcb->rto_deadline && now >= pcb->rto_deadline &&
            SEQ_GT(pcb->snd_nxt, pcb->snd_una)) {
            // Simple retransmit: resend from snd_una.
            uint32_t rlen = pcb->snd_nxt - pcb->snd_una;
            if (rlen > TCP_MSS) rlen = TCP_MSS;
            uint32_t old_nxt = pcb->snd_nxt;
            pcb->snd_nxt = pcb->snd_una;  // rewind
            uint8_t flags = TCP_ACK;
            if (pcb->fin_sent && rlen == 0) flags |= TCP_FIN;
            tcp_send_segment(pcb, flags,
                              pcb->txbuf + (pcb->txbuf_head & TCP_TXBUF_MASK),
                              (uint16_t)rlen);
            if (!rlen && !pcb->fin_sent) pcb->snd_nxt = old_nxt;
            pcb->rto_deadline = now + TCP_RTO_NS;
        }

        // TIME_WAIT expiry.
        if (pcb->state == TCP_TIME_WAIT &&
            now - pcb->timewait_start >= TCP_TIME_WAIT_NS) {
            pcb->state = TCP_CLOSED;
            pcb_wake(pcb);
        }
    }
}

// ── Public PCB management ─────────────────────────────────────────────────

tcp_pcb_t* tcp_pcb_alloc(uint16_t lport) {
    for (uint32_t i = 0; i < TCP_MAX_PCBS; i++) {
        if (!s_pcb_used[i]) {
            s_pcb_used[i] = 1;
            tcp_pcb_t* p = &s_pcbs[i];
            // Zero the PCB.
            uint8_t* b = (uint8_t*)p;
            for (uint32_t j = 0; j < sizeof(tcp_pcb_t); j++) b[j] = 0;
            p->local_ip   = net_our_ip();
            p->local_port = lport;
            p->rcv_wnd    = TCP_WINDOW;
            p->state      = TCP_CLOSED;

            p->txbuf = (uint8_t*)kmalloc(TCP_TXBUF_SIZE);
            p->rxbuf = (uint8_t*)kmalloc(TCP_RXBUF_SIZE);
            if (!p->txbuf || !p->rxbuf) {
                if (p->txbuf) kfree(p->txbuf);
                if (p->rxbuf) kfree(p->rxbuf);
                s_pcb_used[i] = 0;
                return NULL;
            }
            return p;
        }
    }
    return NULL;
}

void tcp_pcb_free(tcp_pcb_t* pcb) {
    if (!pcb) return;
    if (pcb->txbuf) { kfree(pcb->txbuf); pcb->txbuf = NULL; }
    if (pcb->rxbuf) { kfree(pcb->rxbuf); pcb->rxbuf = NULL; }
    uint32_t idx = (uint32_t)(pcb - s_pcbs);
    if (idx < TCP_MAX_PCBS) s_pcb_used[idx] = 0;
}

int tcp_connect(tcp_pcb_t* pcb, uint32_t dst_ip_be, uint16_t dst_port) {
    if (!pcb) return -EINVAL;
    if (pcb->state != TCP_CLOSED) return -EISCONN;
    pcb->remote_ip   = dst_ip_be;
    pcb->remote_port = dst_port;
    pcb->iss         = seq32();
    pcb->snd_una     = pcb->iss;
    pcb->snd_nxt     = pcb->iss;
    pcb->snd_wnd     = TCP_WINDOW;
    pcb->reset       = 0;
    pcb->state       = TCP_SYN_SENT;
    if (tcp_send_segment(pcb, TCP_SYN, NULL, 0) < 0) {
        pcb->state = TCP_CLOSED;
        return -ENOBUFS;
    }
    return 0;
}

int tcp_listen(tcp_pcb_t* pcb) {
    pcb->state = TCP_LISTEN;
    return 0;
}

tcp_pcb_t* tcp_accept(tcp_pcb_t* listener) {
    if (listener->accept_count == 0) return NULL;
    tcp_pcb_t* child = listener->accept_queue[listener->accept_head];
    listener->accept_head = (uint8_t)((listener->accept_head + 1u) % TCP_BACKLOG);
    listener->accept_count--;
    return child;
}

int tcp_send(tcp_pcb_t* pcb, const void* data, uint32_t len, int nonblock) {
    if (!pcb || !data) return -EINVAL;
    if (pcb->reset) return -ECONNRESET;
    // CLOSE_WAIT is valid for sending until the app calls close() — the peer
    // has half-closed its receive side, but we may still have data to send.
    if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT) {
        if (pcb->state == TCP_CLOSED) return -EPIPE;
        return -ENOTCONN;
    }
    if (pcb->fin_sent) return -EPIPE;      // app already shut down the send side
    if (len == 0) return 0;

    const uint8_t* src  = (const uint8_t*)data;
    uint32_t       done = 0;

    while (done < len) {
        // Wait for space in the send buffer.
        while (pcb->txbuf_used >= TCP_TXBUF_SIZE) {
            if (done > 0) return (int)done;    // return partial progress
            if (nonblock) return -EAGAIN;
            pcb->waiter = g_current;
            sched_sleep();
            if (pcb->reset) return -ECONNRESET;
            if (pcb->state == TCP_CLOSED) return -EPIPE;
            if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT)
                return -ENOTCONN;
        }

        uint32_t space = TCP_TXBUF_SIZE - pcb->txbuf_used;
        uint32_t chunk = len - done;
        if (chunk > space) chunk = space;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        for (uint32_t i = 0; i < chunk; i++)
            pcb->txbuf[(pcb->txbuf_tail + i) & TCP_TXBUF_MASK] = src[done + i];
        pcb->txbuf_tail  = (pcb->txbuf_tail + chunk) & TCP_TXBUF_MASK;
        pcb->txbuf_used += chunk;
        done            += chunk;

        // Send what we just buffered if the send window allows.
        uint32_t sendable = pcb->snd_wnd - (pcb->snd_nxt - pcb->snd_una);
        if (sendable > chunk) sendable = chunk;
        if (sendable) {
            uint32_t send_off = (pcb->txbuf_head +
                                 (pcb->snd_nxt - pcb->snd_una)) & TCP_TXBUF_MASK;
            tcp_send_segment(pcb, TCP_ACK | TCP_PSH,
                              pcb->txbuf + send_off, (uint16_t)sendable);
        }
    }
    return (int)done;
}

int tcp_recv_data(tcp_pcb_t* pcb, void* buf, uint32_t len, int nonblock) {
    if (!pcb || !buf) return -EINVAL;
    if (len == 0) return 0;
    uint8_t* dst = (uint8_t*)buf;

    while (pcb->rxbuf_used == 0) {
        // Already at EOF — return 0 bytes so the caller sees end-of-stream.
        if (pcb->fin_rcvd) return 0;
        if (pcb->reset)    return -ECONNRESET;
        if (pcb->state == TCP_CLOSED) return -ENOTCONN;
        if (nonblock)      return -EAGAIN;
        pcb->waiter = g_current;
        sched_sleep();
    }

    uint32_t avail = pcb->rxbuf_used;
    uint32_t copy  = len < avail ? len : avail;
    for (uint32_t i = 0; i < copy; i++)
        dst[i] = pcb->rxbuf[(pcb->rxbuf_head + i) & TCP_RXBUF_MASK];
    pcb->rxbuf_head  = (pcb->rxbuf_head + copy) & TCP_RXBUF_MASK;
    pcb->rxbuf_used -= copy;
    pcb->rcv_wnd    += copy;
    // Send window update ACK so the peer can resume sending.
    tcp_send_segment(pcb, TCP_ACK, NULL, 0);
    return (int)copy;
}

void tcp_close(tcp_pcb_t* pcb) {
    if (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT) {
        uint8_t flags = TCP_FIN | TCP_ACK;
        tcp_send_segment(pcb, flags, NULL, 0);
        pcb->fin_sent = 1;
        pcb->state = (pcb->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1
                                                      : TCP_LAST_ACK;
    } else {
        pcb->state = TCP_CLOSED;
    }
}

tcp_state_t tcp_pcb_state(const tcp_pcb_t* pcb) { return pcb->state; }

void tcp_pcb_set_waiter(tcp_pcb_t* pcb, void* waiter) {
    pcb->waiter = (task_t*)waiter;
}

void tcp_pcb_set_file(tcp_pcb_t* pcb, void* file) {
    if (pcb) pcb->sock_file = file;
}

uint32_t tcp_pcb_rx_used(const tcp_pcb_t* pcb) {
    return pcb ? pcb->rxbuf_used : 0;
}

uint32_t tcp_pcb_tx_space(const tcp_pcb_t* pcb) {
    if (!pcb) return 0;
    return TCP_TXBUF_SIZE - pcb->txbuf_used;
}

int tcp_pcb_eof(const tcp_pcb_t* pcb) {
    if (!pcb) return 1;
    return pcb->fin_rcvd ? 1 : 0;
}

int tcp_pcb_has_accept(const tcp_pcb_t* pcb) {
    return (pcb && pcb->state == TCP_LISTEN && pcb->accept_count > 0) ? 1 : 0;
}

uint16_t tcp_ephemeral_port(void) {
    uint16_t p = s_eph_port;
    s_eph_port = (uint16_t)(s_eph_port == 65535u ? 49152u : s_eph_port + 1u);
    return p;
}
