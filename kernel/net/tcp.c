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
#include "smp.h"
#include "preempt.h"
#include "rcu.h"

// ── TCP constants ─────────────────────────────────────────────────────────
#define TCP_RTO_NS        1000000000ULL   // retransmit timeout: 1 second
// Max SYN-ACK retransmits for a SYN_RCVD half-open before the timer reaps it.
// At TCP_RTO_NS=1s this is ~6 s of grace before an un-completed, un-accepted
// half-open is freed -- enough for a slow legitimate client, but it bounds a
// remote SYN flood (each withheld-final-ACK SYN otherwise leaks ~128 KiB).
#define TCP_SYN_RCVD_MAX_RETRIES 5u
#define TCP_MSS           1460u           // max segment size (1500 - 20 IP - 20 TCP)
#define TCP_WINDOW        65535u          // advertised receive window
#define TCP_TIME_WAIT_NS  4000000000ULL   // TIME_WAIT: 2×MSL = 4 seconds
// TCP_MAX_PCBS removed — PCBs are now kmalloc'd and linked; no fixed cap.
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
    // SYN-ACK retransmit count for a SYN_RCVD half-open.  Incremented by the
    // timer thread each time the SYN-ACK is resent; past TCP_SYN_RCVD_MAX_RETRIES
    // an UN-ACCEPTED half-open (sock_file==NULL) is reaped, so a remote SYN that
    // never completes cannot leak its ~128 KiB forever (a SYN-flood DoS).  Only
    // the timer thread writes it; no lock needed for the counter itself.
    uint8_t  rexmit;
    // Set by the timer reaper UNDER s_pcb_wlock when it claims a half-open for
    // freeing.  tcp_recv's handshake completion checks it (also under
    // s_pcb_wlock) and aborts the establish, so a child the reaper is freeing is
    // never moved to ESTABLISHED / pushed onto an accept queue (no UAF of a
    // being-reaped child).
    uint8_t  reaped;

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

    // Serializes the ring accounting (tx/rx used + indices + snd_una) between
    // the RX kernel thread and the socket syscall path.  See tcp_pcb_lock.
    spinlock_t lock;

    // Intrusive singly-linked list node for s_pcb_head.
    struct tcp_pcb* next;
    // Transient link used ONLY by the timer reaper to chain claimed half-opens
    // for a batched free after one RCU grace period.  Separate from `next` so a
    // concurrent RCU reader still walking `next` is undisturbed by the chaining.
    struct tcp_pcb* reap_next;
};

// ── PCB list ──────────────────────────────────────────────────────────────
// Singly-linked list of all live PCBs.  Insertion is O(1); lookup is O(n)
// in the number of open connections — same complexity as before but with no
// fixed cap.  Each PCB is individually kmalloc'd.
// RCU-protected singly-linked list of live PCBs.  Readers (pcb_find,
// tcp_timer_tick) walk under rcu_read_lock() with zero synchronization.
// Writers serialise on s_pcb_wlock for insert/remove of the ->next link,
// and tcp_pcb_free defers the actual teardown to call_rcu so a concurrent
// reader that still holds a pointer to the pcb sees a valid object until
// its rcu_read_unlock.
static tcp_pcb_t* s_pcb_head   = NULL;
static spinlock_t s_pcb_wlock  = SPINLOCK_INIT;

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

// Consume `n` bytes from the read side of a tx/rx ring: advance *head (masked)
// and decrement *used, CLAMPING n to *used so an over-consume can never
// underflow the count.  Single source of truth for both the txbuf ACK drain
// and the rxbuf read drain.  Defense-in-depth: a raced ACK accounting update
// (T4 -- snd_una/txbuf_used RMW'd from another CPU with no per-PCB lock yet) or
// a malformed segment could otherwise make `n` exceed *used, underflowing it to
// ~4e9 -- which wedges the producer's `while used >= SIZE` wait or drives a wild
// length.  Clamping bounds the worst case to a transient mis-account.  Pure ->
// unit-tested (tcp_ring_consume_selftest).  Returns the bytes actually consumed.
static uint32_t tcp_ring_consume(uint32_t* head, uint32_t* used,
                                 uint32_t n, uint32_t mask) {
    if (n > *used) n = *used;            // clamp: never underflow the count
    *head = (*head + n) & mask;
    *used -= n;
    return n;
}

// Reserve up to `want` bytes on the WRITE side of a tx/rx ring: clamp to the
// free space (size - *used), set *out_start to the old tail, advance *tail
// (masked) and *used by the reserved amount, return the amount reserved.  The
// producer mirror of tcp_ring_consume.  Reserving the index range UNDER the
// per-PCB lock and copying the payload into [out_start, +n) OUTSIDE the lock
// keeps a user-memory copy out of the critical section (no fault under a
// spinlock) and is multi-producer-safe (disjoint reserved ranges).  Pure ->
// unit-tested (tcp_ring_reserve_selftest).
static uint32_t tcp_ring_reserve(uint32_t* tail, uint32_t* used, uint32_t want,
                                 uint32_t size, uint32_t mask, uint32_t* out_start) {
    uint32_t space = size - *used;       // invariant: *used <= size
    uint32_t n     = want < space ? want : space;
    *out_start = *tail;
    *tail = (*tail + n) & mask;
    *used += n;
    return n;
}

// ── Per-PCB lock ───────────────────────────────────────────────────────────
// Serializes the send/receive ring accounting (txbuf_used / rxbuf_used + the
// head/tail indices + snd_una) between the RX path (the net RX kernel thread)
// and the socket syscall path (send/recv on user CPUs).  RX runs in a kernel
// thread (the virtio_net IRQ only irq_notify's; it is never an IRQ handler),
// so a plain spinlock under preempt_disable is sufficient -- NO irqsave (the
// F35 pattern).  LEAF lock: held ONLY around the field mutations; pcb_wake
// (rq_lock), tcp_send_segment (I/O), the user payload copy, and sched_sleep all
// stay OUTSIDE it.  NOTE (T4 remaining): snd_nxt (advanced by tcp_send_segment
// while it also does I/O) and the state-machine transitions are NOT yet under
// this lock -- those are single-writer-dominant and benign-read races, a
// separate follow-up; this lock closes the documented ring-accounting race.
static inline void tcp_pcb_lock(tcp_pcb_t* pcb)   { preempt_disable(); spin_lock(&pcb->lock); }
static inline void tcp_pcb_unlock(tcp_pcb_t* pcb) { spin_unlock(&pcb->lock); preempt_enable(); }

// ── Listener accept-queue ring (single source of truth) ────────────────────
// The completed-connection FIFO on a LISTEN pcb: the RX kernel thread enqueues
// a child that finished its handshake (SYN_RCVD -> ESTABLISHED) while accept()
// on a user CPU dequeues -- so accept_head/accept_tail/accept_count race and
// the uint8_t count can tear (lost child, double-dequeue).  These two helpers
// are the ONLY mutators of the ring; the CALLER must hold the LISTENER's
// pcb->lock around them (the queue belongs to the listener, so it is a single
// lock -- no child<->listener two-lock cycle).  accept_q_push returns false if
// the backlog is full (the child is dropped, as before).
static bool accept_q_push(tcp_pcb_t* lst, tcp_pcb_t* child) {
    if (lst->accept_count >= TCP_BACKLOG) return false;     // full -> drop
    lst->accept_queue[lst->accept_tail] = child;
    lst->accept_tail = (uint8_t)((lst->accept_tail + 1u) % TCP_BACKLOG);
    lst->accept_count++;
    return true;
}
static tcp_pcb_t* accept_q_pop(tcp_pcb_t* lst) {
    if (lst->accept_count == 0) return NULL;                 // empty
    tcp_pcb_t* child = lst->accept_queue[lst->accept_head];
    lst->accept_head = (uint8_t)((lst->accept_head + 1u) % TCP_BACKLOG);
    lst->accept_count--;
    return child;
}

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
    void* sf = __atomic_load_n(&pcb->sock_file, __ATOMIC_ACQUIRE);
    if (sf) {
        // sf is the socket's vfs_file_t.  This wake runs inside the tcp_recv /
        // tcp_timer_tick rcu_read_lock section, and sock_close defers the file
        // free to sock_free_rcu, so f outlives this reader -- no UAF write.
        vfs_file_t* f = (vfs_file_t*)sf;
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
        __builtin_memcpy(p, data, dlen);
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

// Caller must be inside rcu_read_lock() and stay there for as long as it
// uses the returned pcb.  That keeps the pcb alive even if another CPU
// calls tcp_pcb_free — the actual free is deferred via call_rcu until
// every pre-existing reader has dropped out.
static tcp_pcb_t* pcb_find(uint32_t local_ip, uint16_t local_port,
                             uint32_t remote_ip, uint16_t remote_port) {
    (void)local_ip;
    tcp_pcb_t* listener = NULL;
    for (tcp_pcb_t* p = rcu_dereference(s_pcb_head); p;
                    p = rcu_dereference(p->next)) {
        // Exact match (ESTABLISHED / SYN_RCVD / etc).
        if (p->local_port  == local_port  &&
            p->remote_port == remote_port &&
            p->remote_ip   == remote_ip   &&
            p->state       != TCP_LISTEN)
            return p;
        // Listener match (local port, wildcard remote).
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

    // RCU reader section covers pcb_find and every dereference below.
    // call_rcu in tcp_pcb_free keeps the pcb alive until we rcu_read_unlock,
    // which happens at the single exit at the bottom of this function.
    rcu_read_lock();
    tcp_pcb_t* pcb = pcb_find(our_ip, dst_port, skb->src_ip_be, src_port);

    if (!pcb) {
        rcu_read_unlock();
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
        rcu_read_unlock();
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
            // Commit the establish atomically vs the timer half-open reaper: if
            // the reaper already claimed this half-open (over its SYN-ACK
            // retransmit cap) it set ->reaped + unlinked it under s_pcb_wlock and
            // is freeing it -- do NOT establish or queue a child that is being
            // reaped (that would push a freed pcb onto the accept queue).  The
            // reaper, also under s_pcb_wlock, observes ESTABLISHED here and skips
            // it.  The accept_q_push stays under the LISTENER's lock (separate
            // critical section; no two-lock nesting).
            int establish = 0;
            uint64_t wf = spin_lock_irqsave(&s_pcb_wlock);
            if (!pcb->reaped && pcb->state == TCP_SYN_RCVD) {
                pcb->snd_una = ntoh32(seg->ack);
                pcb->state   = TCP_ESTABLISHED;
                establish = 1;
            }
            spin_unlock_irqrestore(&s_pcb_wlock, wf);
            // Move to listener's accept queue under the LISTENER's lock (it
            // races accept()'s dequeue); wake accept() OUTSIDE the lock.
            tcp_pcb_t* lst = pcb->listener;
            if (establish && lst) {
                tcp_pcb_lock(lst);
                bool queued = accept_q_push(lst, pcb);
                tcp_pcb_unlock(lst);
                if (queued) pcb_wake(lst);
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
            tcp_pcb_lock(pcb);               // serialize txbuf_used + snd_una vs send()
            uint32_t acked = ack - pcb->snd_una;
            tcp_ring_consume(&pcb->txbuf_head, &pcb->txbuf_used,
                             acked, TCP_TXBUF_MASK);
            pcb->snd_una     = ack;
            pcb->snd_wnd     = ntoh16(seg->window);
            if (pcb->snd_una == pcb->snd_nxt) pcb->rto_deadline = 0;
            tcp_pcb_unlock(pcb);
            pcb_wake(pcb);                   // wake OUTSIDE the lock (takes rq_lock)
        }

        // Receive in-order data.
        if (dlen > 0 && seq == pcb->rcv_nxt) {
            // The payload copy is kernel skbuff -> kernel rxbuf, so it is safe
            // under the lock; serialize rxbuf_used/tail vs recv().
            tcp_pcb_lock(pcb);
            uint32_t space = TCP_RXBUF_SIZE - pcb->rxbuf_used;
            uint32_t copy  = dlen < space ? dlen : space;
            for (uint32_t i = 0; i < copy; i++)
                pcb->rxbuf[(pcb->rxbuf_tail + i) & TCP_RXBUF_MASK] = data[i];
            pcb->rxbuf_tail  = (pcb->rxbuf_tail + copy) & TCP_RXBUF_MASK;
            pcb->rxbuf_used += copy;
            pcb->rcv_nxt    += copy;
            tcp_pcb_unlock(pcb);
            pcb_wake(pcb);                   // wake + ACK OUTSIDE the lock (rq_lock / I/O)
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

    rcu_read_unlock();
    skb_free(skb);
}

// ── Timer tick ────────────────────────────────────────────────────────────

static void tcp_pcb_free_rcu(void* data);   // defined below; used by the reaper

void tcp_timer_tick(void) {
    uint64_t now = tsc_read_ns();
    // RCU reader section: a concurrent tcp_pcb_free can unlink a pcb we're
    // walking, but call_rcu defers the actual free until after we drop the
    // reader section, so every pcb we touch stays valid.
    rcu_read_lock();
    for (tcp_pcb_t* pcb = rcu_dereference(s_pcb_head); pcb;
                    pcb = rcu_dereference(pcb->next)) {

        // Retransmit timeout.
        if (pcb->rto_deadline && now >= pcb->rto_deadline &&
            SEQ_GT(pcb->snd_nxt, pcb->snd_una)) {
            // SYN_SENT / SYN_RCVD: the outstanding "byte" is the SYN itself,
            // which must be retransmitted as a SYN (with ACK in SYN_RCVD),
            // never as a plain ACK.  Blindly sending TCP_ACK here wedged the
            // handshake whenever the first SYN's ipv4_send_ex failed on an
            // ARP miss — the retry looked like an ACK-only segment, peer
            // dropped it, and connect() hit the caller's timeout.
            uint8_t flags;
            uint32_t rlen = 0;
            const void* rdata = NULL;
            uint32_t old_nxt = pcb->snd_nxt;
            if (pcb->state == TCP_SYN_SENT) {
                flags = TCP_SYN;
                pcb->snd_nxt = pcb->snd_una;  // rewind over SYN
            } else if (pcb->state == TCP_SYN_RCVD) {
                flags = TCP_SYN | TCP_ACK;
                pcb->snd_nxt = pcb->snd_una;
                // Count SYN-ACK retransmits of an UN-ACCEPTED half-open so the
                // reaper below can free it once it exceeds the cap (only the
                // timer thread writes rexmit -> no lock needed).
                if (!pcb->sock_file && pcb->rexmit < 255u) pcb->rexmit++;
            } else {
                // ESTABLISHED / FIN_WAIT / CLOSE_WAIT / ...: resend data.
                rlen = pcb->snd_nxt - pcb->snd_una;
                if (rlen > TCP_MSS) rlen = TCP_MSS;
                pcb->snd_nxt = pcb->snd_una;
                flags = TCP_ACK;
                if (pcb->fin_sent && rlen == 0) flags |= TCP_FIN;
                rdata = pcb->txbuf + (pcb->txbuf_head & TCP_TXBUF_MASK);
            }
            tcp_send_segment(pcb, flags, rdata, (uint16_t)rlen);
            if (!rlen && !pcb->fin_sent && pcb->state != TCP_SYN_SENT &&
                pcb->state != TCP_SYN_RCVD)
                pcb->snd_nxt = old_nxt;
            pcb->rto_deadline = now + TCP_RTO_NS;
        }

        // TIME_WAIT expiry.
        if (pcb->state == TCP_TIME_WAIT &&
            now - pcb->timewait_start >= TCP_TIME_WAIT_NS) {
            pcb->state = TCP_CLOSED;
            pcb_wake(pcb);
        }
    }
    rcu_read_unlock();

    // ── Reap SYN_RCVD half-opens past the SYN-ACK retransmit cap ─────────────
    // A SYN_RCVD child with no socket (never accepted) whose final ACK never
    // arrives would resend its SYN-ACK forever and never be freed -- an
    // unbounded remote SYN-flood kernel-memory leak (~128 KiB each).  This MUST
    // run OUTSIDE the rcu_read_lock above: tcp_pcb_free_rcu is reached via a
    // grace period, and synchronize_rcu_expedited PANICS if called under a
    // reader / preempt-disabled.  Claim each victim UNDER s_pcb_wlock (atomic vs
    // tcp_recv's handshake completion, which sets ESTABLISHED under the same
    // lock and checks ->reaped): set ->reaped (so a racing establish aborts),
    // unlink it (leaving its own ->next intact so a concurrent reader walking
    // ->next is undisturbed), and chain it via ->reap_next.  Then ONE grace
    // period covers the whole batch before the direct frees.
    tcp_pcb_t* reap = NULL;
    uint64_t f = spin_lock_irqsave(&s_pcb_wlock);
    tcp_pcb_t* prev = NULL;
    for (tcp_pcb_t* p = s_pcb_head; p; ) {
        tcp_pcb_t* pn = p->next;
        if (p->state == TCP_SYN_RCVD && !p->sock_file &&
            p->rexmit > TCP_SYN_RCVD_MAX_RETRIES) {
            p->reaped = 1;
            if (prev) rcu_assign_pointer(prev->next, pn);
            else      rcu_assign_pointer(s_pcb_head, pn);
            p->reap_next = reap;   // chain (separate field; ->next left intact)
            reap = p;
            // prev unchanged -- p is unlinked
        } else {
            prev = p;
        }
        p = pn;
    }
    spin_unlock_irqrestore(&s_pcb_wlock, f);

    if (reap) {
        synchronize_rcu_expedited();   // one grace period for the whole batch
        while (reap) {
            tcp_pcb_t* nx = reap->reap_next;
            tcp_pcb_free_rcu(reap);    // grace period elapsed -> free directly
            reap = nx;
        }
    }
}

// ── Public PCB management ─────────────────────────────────────────────────

tcp_pcb_t* tcp_pcb_alloc(uint16_t lport) {
    tcp_pcb_t* p = (tcp_pcb_t*)kmalloc(sizeof(tcp_pcb_t));
    if (!p) return NULL;

    // Zero the PCB.
    __builtin_memset(p, 0, sizeof(tcp_pcb_t));
    spin_lock_init(&p->lock);
    p->local_ip   = net_our_ip();
    p->local_port = lport;
    p->rcv_wnd    = TCP_WINDOW;
    p->state      = TCP_CLOSED;

    p->txbuf = (uint8_t*)kmalloc(TCP_TXBUF_SIZE);
    p->rxbuf = (uint8_t*)kmalloc(TCP_RXBUF_SIZE);
    if (!p->txbuf || !p->rxbuf) {
        if (p->txbuf) kfree(p->txbuf);
        if (p->rxbuf) kfree(p->rxbuf);
        kfree(p);
        return NULL;
    }

    // Publish the new pcb at the head of the RCU-protected list.  Readers
    // that have already loaded s_pcb_head see their old snapshot; readers
    // that load after rcu_assign_pointer see the new head.  Either is
    // correct — a new pcb has no traffic yet.
    uint64_t flags = spin_lock_irqsave(&s_pcb_wlock);
    p->next = s_pcb_head;
    rcu_assign_pointer(s_pcb_head, p);
    spin_unlock_irqrestore(&s_pcb_wlock, flags);
    serial_puts_dbg("[tcp] pcb_alloc port=");
    serial_hex_dbg((uint64_t)lport);
    return p;
}

// Deferred teardown: runs after a full RCU grace period, at which point
// no pcb_find / tcp_recv / tcp_timer_tick reader still holds a pointer
// to this pcb.  Safe to free the buffers and the pcb itself.
static void tcp_pcb_free_rcu(void* data) {
    tcp_pcb_t* pcb = (tcp_pcb_t*)data;
    if (pcb->txbuf) { kfree(pcb->txbuf); pcb->txbuf = NULL; }
    if (pcb->rxbuf) { kfree(pcb->rxbuf); pcb->rxbuf = NULL; }
    serial_puts_dbg("[tcp] pcb_free port=");
    serial_hex_dbg((uint64_t)pcb->local_port);
    kfree(pcb);
}

void tcp_pcb_free(tcp_pcb_t* pcb) {
    if (!pcb) return;
    // Unlink from the RCU-protected list.  Writer-side exclusion via
    // s_pcb_wlock; the ->next pointer updates are plain stores because
    // readers walk via rcu_dereference which on x86 is a plain load.
    uint64_t flags = spin_lock_irqsave(&s_pcb_wlock);
    // Orphan any SYN_RCVD child that still points back at `pcb` as its listener.
    // Once `pcb` is freed below, a child's late handshake-completing ACK would
    // otherwise read the dangling child->listener and lock + push the accept
    // queue of freed memory (a remote-timed heap UAF write, tcp_recv's
    // TCP_SYN_RCVD case).  NULLing the backpointer makes that path skip the dead
    // listener (its `if (lst)` guard).  No-op when `pcb` is not a listener (no
    // child references it).  Under s_pcb_wlock (held for the unlink) so it
    // cannot race a concurrent list alloc/free; a concurrent RX reader that
    // already loaded the old child->listener is safe because `pcb`'s own free is
    // RCU-deferred and keeps it alive until that reader drops its rcu section.
    for (tcp_pcb_t* p = s_pcb_head; p; p = p->next) {
        if (p->listener == pcb) p->listener = NULL;
    }
    if (s_pcb_head == pcb) {
        rcu_assign_pointer(s_pcb_head, pcb->next);
    } else {
        for (tcp_pcb_t* p = s_pcb_head; p; p = p->next) {
            if (p->next == pcb) {
                rcu_assign_pointer(p->next, pcb->next);
                break;
            }
        }
    }
    spin_unlock_irqrestore(&s_pcb_wlock, flags);
    // Defer the actual free until every in-flight reader has dropped
    // its reference.  Expedited: close() on a TCP socket releases its
    // PCB here, on the user-syscall return path.
    call_rcu_expedited(tcp_pcb_free_rcu, pcb);
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
    int r = tcp_send_segment(pcb, TCP_SYN, NULL, 0);
    // r < 0 usually means ARP miss on the first try (ipv4_send_ex returns -1
    // while the ARP request is still outstanding).  The segment was still
    // logically queued — snd_nxt advanced and rto_deadline is armed — so the
    // retransmit tick will resend once ARP resolves.  Hard-fail only if we
    // couldn't even build the segment (rto_deadline stays 0).
    if (r < 0 && pcb->rto_deadline == 0) {
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
    // Dequeue under the listener's lock (races the RX thread's accept_q_push).
    tcp_pcb_lock(listener);
    tcp_pcb_t* child = accept_q_pop(listener);
    tcp_pcb_unlock(listener);
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

        // Reserve a chunk of tx-ring space UNDER the lock (clamps to free
        // space), then copy the user payload into the reserved range OUTSIDE
        // the lock: src may be user memory (so the copy must not run under a
        // spinlock), and the RX ACK path only advances txbuf_head, never the
        // reserved tail range, so this is safe and multi-producer-correct.
        uint32_t want = len - done;
        if (want > TCP_MSS) want = TCP_MSS;
        uint32_t tail0;
        tcp_pcb_lock(pcb);
        uint32_t chunk = tcp_ring_reserve(&pcb->txbuf_tail, &pcb->txbuf_used,
                                          want, TCP_TXBUF_SIZE, TCP_TXBUF_MASK,
                                          &tail0);
        tcp_pcb_unlock(pcb);
        if (chunk == 0) continue;          // raced full -> re-enter the wait loop
        for (uint32_t i = 0; i < chunk; i++)
            pcb->txbuf[(tail0 + i) & TCP_TXBUF_MASK] = src[done + i];
        done += chunk;

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

    for (;;) {
        while (pcb->rxbuf_used == 0) {
            // Already at EOF — return 0 bytes so the caller sees end-of-stream.
            if (pcb->fin_rcvd) return 0;
            if (pcb->reset)    return -ECONNRESET;
            if (pcb->state == TCP_CLOSED) return -ENOTCONN;
            if (nonblock)      return -EAGAIN;
            pcb->waiter = g_current;
            sched_sleep();
        }

        // Reserve the read range UNDER the lock (snapshot head, advance
        // head/used via the clamped helper), then copy out to the (possibly
        // user) dst OUTSIDE the lock.  Multi-consumer-safe: concurrent recv()s
        // reserve disjoint ranges; the clamp keeps copy <= used.
        tcp_pcb_lock(pcb);
        uint32_t avail = pcb->rxbuf_used;
        if (avail == 0) { tcp_pcb_unlock(pcb); continue; }  // raced empty -> re-wait
        uint32_t copy  = len < avail ? len : avail;
        uint32_t head0 = pcb->rxbuf_head;
        tcp_ring_consume(&pcb->rxbuf_head, &pcb->rxbuf_used, copy, TCP_RXBUF_MASK);
        pcb->rcv_wnd += copy;
        tcp_pcb_unlock(pcb);

        for (uint32_t i = 0; i < copy; i++)
            dst[i] = pcb->rxbuf[(head0 + i) & TCP_RXBUF_MASK];
        // Send window update ACK so the peer can resume sending.
        tcp_send_segment(pcb, TCP_ACK, NULL, 0);
        return (int)copy;
    }
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
    // RELEASE store: (un)publishes the poll backpointer so a pcb_wake reader
    // either observes the file (and is covered by the file's RCU grace period,
    // sock_free_rcu) or observes NULL and skips the wake.  Pairs with the
    // ACQUIRE load in pcb_wake.
    if (pcb) __atomic_store_n(&pcb->sock_file, file, __ATOMIC_RELEASE);
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

// ── tcp_ring_consume selftest ─────────────────────────────────────────────
// Deterministic check of the tx/rx ring drain arithmetic + the underflow
// clamp that bounds the worst case of the (still-open) T4 PCB data race.
void tcp_ring_consume_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    struct { uint32_t head0, used0, n, mask, ehead, eused, eret; } c[] = {
        // head, used, n, mask=0xFF (256-byte ring) -> expected head/used/ret
        {   0, 100,  30, 0xFFu,  30,  70, 30 },  // normal drain
        { 250, 100,  20, 0xFFu,  14,  80, 20 },  // head wraps: (250+20)&255 = 14
        {   0,  10,  50, 0xFFu,  10,   0, 10 },  // OVER-consume -> clamp to used, no underflow
        {   5,   5,   5, 0xFFu,  10,   0,  5 },  // exact drain to empty
        {  64,   0,  17, 0xFFu,  64,   0,  0 },  // already empty -> consume 0, no underflow
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        uint32_t head = c[i].head0, used = c[i].used0;
        uint32_t ret = tcp_ring_consume(&head, &used, c[i].n, c[i].mask);
        if (head != c[i].ehead || used != c[i].eused || ret != c[i].eret) {
            kprintf("[tcp_ring] FAIL i=%u head=%lu(want %lu) used=%lu(want %lu) ret=%lu(want %lu)\n",
                    i, (unsigned long)head, (unsigned long)c[i].ehead,
                    (unsigned long)used, (unsigned long)c[i].eused,
                    (unsigned long)ret, (unsigned long)c[i].eret);
            fails++;
        }
    }
    kprintf(fails ? "[tcp_ring] SELF-TEST FAILED\n"
                  : "[tcp_ring] SELF-TEST PASSED (ring drain + underflow clamp)\n");
}

// ── tcp_ring_reserve selftest ─────────────────────────────────────────────
// Deterministic check of the producer-side ring reserve arithmetic that the
// per-PCB-lock send/rx-write paths use: the reserved range starts at the old
// tail, is clamped to the free space (size - used), and advances tail (masked)
// and used by the reserved amount.  Pairs with tcp_ring_consume_selftest.
void tcp_ring_reserve_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    struct { uint32_t tail0, used0, want, size, mask, estart, etail, eused, eret; } c[] = {
        {   0,   0,  30, 256, 0xFFu,   0,  30,  30, 30 },  // normal reserve
        { 250,   0,  20, 256, 0xFFu, 250,  14,  20, 20 },  // tail wraps: (250+20)&255 = 14
        {   0, 250,  50, 256, 0xFFu,   0,   6, 256,  6 },  // clamp to free space (6)
        { 100, 256,  10, 256, 0xFFu, 100, 100, 256,  0 },  // full -> reserve 0
        { 200, 200, 100, 256, 0xFFu, 200,   0, 256, 56 },  // wrap + clamp: free = 56
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        uint32_t tail = c[i].tail0, used = c[i].used0, start = 0xDEADu;
        uint32_t ret = tcp_ring_reserve(&tail, &used, c[i].want, c[i].size,
                                        c[i].mask, &start);
        if (start != c[i].estart || tail != c[i].etail ||
            used != c[i].eused || ret != c[i].eret) {
            kprintf("[tcp_reserve] FAIL i=%u start=%lu tail=%lu used=%lu ret=%lu\n",
                    i, (unsigned long)start, (unsigned long)tail,
                    (unsigned long)used, (unsigned long)ret);
            fails++;
        }
    }
    kprintf(fails ? "[tcp_reserve] SELF-TEST FAILED\n"
                  : "[tcp_reserve] SELF-TEST PASSED (ring reserve, clamp + wrap)\n");
}

// ── accept-queue ring selftest ────────────────────────────────────────────
// Deterministic check of the listener accept-queue push/pop ring (now taken
// under the listener's pcb->lock at both the RX enqueue and accept() dequeue):
// FIFO order, full-rejects, empty->NULL, and wrap-around across the modulo.
// Uses a kmalloc'd fake listener and distinct non-NULL marker pointers (never
// dereferenced -- only compared for identity).
void tcp_accept_q_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    tcp_pcb_t* lst = (tcp_pcb_t*)kmalloc(sizeof(tcp_pcb_t));
    if (!lst) { kprintf("[tcp_accept] SELF-TEST SKIP (no mem)\n"); return; }
    __builtin_memset(lst, 0, sizeof(*lst));     // head = tail = count = 0
    tcp_pcb_t* mark[TCP_BACKLOG];
    for (unsigned i = 0; i < TCP_BACKLOG; i++)
        mark[i] = (tcp_pcb_t*)(uintptr_t)(0x1000u + i);  // identity-only markers

    if (accept_q_pop(lst) != NULL) { kprintf("[tcp_accept] FAIL pop empty\n"); fails++; }

    // Fill to the backlog; one more must be rejected.
    for (unsigned i = 0; i < TCP_BACKLOG; i++)
        if (!accept_q_push(lst, mark[i])) { kprintf("[tcp_accept] FAIL push %u\n", i); fails++; }
    if (lst->accept_count != TCP_BACKLOG) { kprintf("[tcp_accept] FAIL count\n"); fails++; }
    if (accept_q_push(lst, mark[0]))      { kprintf("[tcp_accept] FAIL push full\n"); fails++; }

    // Drain in FIFO order to empty.
    for (unsigned i = 0; i < TCP_BACKLOG; i++)
        if (accept_q_pop(lst) != mark[i]) { kprintf("[tcp_accept] FAIL FIFO %u\n", i); fails++; }
    if (lst->accept_count != 0 || accept_q_pop(lst) != NULL) {
        kprintf("[tcp_accept] FAIL drained\n"); fails++; }

    // Wrap-around: push+pop many times past the modulo boundary, stays FIFO.
    for (unsigned r = 0; r < TCP_BACKLOG * 3u; r++) {
        if (!accept_q_push(lst, mark[r % TCP_BACKLOG]) ||
            accept_q_pop(lst) != mark[r % TCP_BACKLOG]) {
            kprintf("[tcp_accept] FAIL wrap r=%u\n", r); fails++; }
    }
    if (lst->accept_count != 0) { kprintf("[tcp_accept] FAIL wrap count\n"); fails++; }

    kfree(lst);
    kprintf(fails ? "[tcp_accept] SELF-TEST FAILED\n"
                  : "[tcp_accept] SELF-TEST PASSED (accept-queue ring: FIFO, full, empty, wrap)\n");
}

// Deterministic test of the listener->child orphaning (the child->listener UAF
// fix): freeing a listener PCB must NULL the ->listener backpointer of every
// SYN_RCVD child on the live list (so the child's later handshake ACK skips the
// freed listener), and freeing an UNRELATED pcb must NOT disturb a child.  Uses
// the real alloc/free path on high test ports; the concurrent late-ACK race is
// covered by code-proof (RCU keeps a concurrent reader's listener alive).
void tcp_listener_orphan_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    tcp_pcb_t* lst   = tcp_pcb_alloc(0xFFFE);
    tcp_pcb_t* child = tcp_pcb_alloc(0xFFFE);
    if (!lst || !child) {
        if (lst)   tcp_pcb_free(lst);
        if (child) tcp_pcb_free(child);
        kprintf("[tcp_orphan] SELF-TEST SKIP (no mem)\n");
        return;
    }
    lst->state      = TCP_LISTEN;
    child->state    = TCP_SYN_RCVD;
    child->listener = lst;

    // 1. Freeing the listener clears the child's backpointer.
    tcp_pcb_free(lst);
    if (child->listener != NULL) {
        fails++;
        kprintf("[tcp_orphan] FAIL child->listener not cleared on listener free\n");
    }

    // 2. Freeing an UNRELATED pcb must NOT clear a child's backpointer, and the
    //    matching listener free must.
    tcp_pcb_t* lst2  = tcp_pcb_alloc(0xFFFD);
    tcp_pcb_t* other = tcp_pcb_alloc(0xFFFC);
    if (lst2 && other) {
        child->listener = lst2;
        tcp_pcb_free(other);                    // unrelated -> must not touch child
        if (child->listener != lst2) {
            fails++;
            kprintf("[tcp_orphan] FAIL unrelated free cleared child->listener\n");
        }
        tcp_pcb_free(lst2);                      // matching -> must clear
        if (child->listener != NULL) {
            fails++;
            kprintf("[tcp_orphan] FAIL second listener free did not clear\n");
        }
    } else {
        if (lst2)  tcp_pcb_free(lst2);
        if (other) tcp_pcb_free(other);
    }

    tcp_pcb_free(child);   // cleanup
    kprintf(fails ? "[tcp_orphan] SELF-TEST FAILED\n"
                  : "[tcp_orphan] SELF-TEST PASSED (listener free orphans SYN_RCVD children)\n");
}

// Walk s_pcb_head and report whether `target` is still on it.  Pointer-compare
// only -- safe even if `target` was just RCU-freed (we never deref it).
static int tcp_pcb_on_list(tcp_pcb_t* target) {
    int found = 0;
    rcu_read_lock();
    for (tcp_pcb_t* p = rcu_dereference(s_pcb_head); p; p = rcu_dereference(p->next))
        if (p == target) { found = 1; break; }
    rcu_read_unlock();
    return found;
}

// Verify the SYN_RCVD half-open reaper: a child past the SYN-ACK retransmit cap
// with NO socket (never accepted) is freed by tcp_timer_tick (so a remote SYN
// flood cannot leak kernel memory unbounded); a child WITH a socket is never
// reaped.  Single-threaded, so tcp_timer_tick's reap-pass synchronize_rcu_
// expedited has no concurrent reader and cannot deadlock.  Robust against the
// live net_tcp_timer_thread: it reaps the same orphan (same outcome) and never
// touches the owned child (sock_file != NULL).
void tcp_synreap_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;

    // (A) an over-cap SYN_RCVD orphan (sock_file==NULL) gets reaped.
    tcp_pcb_t* orphan = tcp_pcb_alloc(0xFEED);
    if (!orphan) { kprintf("[tcp_synreap] SELF-TEST SKIP (no mem)\n"); return; }
    orphan->state     = TCP_SYN_RCVD;
    orphan->sock_file = NULL;
    orphan->rexmit    = (uint8_t)(TCP_SYN_RCVD_MAX_RETRIES + 1u);   // over the cap
    tcp_timer_tick();                                               // reap-pass frees it
    if (tcp_pcb_on_list(orphan)) {
        fails++;
        kprintf("[tcp_synreap] FAIL over-cap orphan not reaped\n");
        tcp_pcb_free(orphan);   // not reaped -> free so the test does not leak
    }

    // (B) a SYN_RCVD child WITH a socket is NEVER reaped, even over the cap.
    tcp_pcb_t* owned = tcp_pcb_alloc(0xFEEE);
    if (owned) {
        owned->state     = TCP_SYN_RCVD;
        owned->sock_file = (void*)owned;      // any non-NULL = "has a socket owner"
        owned->rexmit    = (uint8_t)(TCP_SYN_RCVD_MAX_RETRIES + 1u);
        tcp_timer_tick();
        if (!tcp_pcb_on_list(owned)) {
            fails++;
            kprintf("[tcp_synreap] FAIL child with a socket was reaped\n");
        } else {
            owned->sock_file = NULL;
            tcp_pcb_free(owned);              // cleanup
        }
    }

    kprintf(fails ? "[tcp_synreap] SELF-TEST FAILED\n"
                  : "[tcp_synreap] SELF-TEST PASSED (over-cap SYN_RCVD half-open reaped, owned child kept)\n");
}
