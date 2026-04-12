#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "skbuff.h"
#include "common.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "errno.h"
#include "syscall.h"   // POLLIN / POLLOUT / POLLHUP / POLLERR, O_NONBLOCK

// ── UDP socket registry ───────────────────────────────────────────────────
// Simple fixed-size table: maps local port → socket_t*.
// Only populated while a UDP socket is open and bound.

#define UDP_SOCK_TABLE_SIZE  64u
#define UDP_RX_QUEUE_MAX     64u   // per-socket cap on pending datagrams

typedef struct {
    uint16_t  port;   // host byte order; 0 = empty slot
    socket_t* sock;
} udp_sock_entry_t;

static udp_sock_entry_t s_udp_table[UDP_SOCK_TABLE_SIZE];

static int udp_table_register(uint16_t port, socket_t* s) {
    // Reject duplicates (EADDRINUSE).
    for (uint32_t i = 0; i < UDP_SOCK_TABLE_SIZE; i++) {
        if (s_udp_table[i].port == port) return -EADDRINUSE;
    }
    for (uint32_t i = 0; i < UDP_SOCK_TABLE_SIZE; i++) {
        if (s_udp_table[i].port == 0) {
            s_udp_table[i].port = port;
            s_udp_table[i].sock = s;
            return 0;
        }
    }
    return -ENOBUFS;
}

static void udp_table_remove(uint16_t port) {
    for (uint32_t i = 0; i < UDP_SOCK_TABLE_SIZE; i++) {
        if (s_udp_table[i].port == port) {
            s_udp_table[i].port = 0;
            s_udp_table[i].sock = 0;
            return;
        }
    }
}

static socket_t* udp_table_find(uint16_t port) {
    for (uint32_t i = 0; i < UDP_SOCK_TABLE_SIZE; i++) {
        if (s_udp_table[i].port == port)
            return s_udp_table[i].sock;
    }
    return 0;
}

// Wake the task sitting in poll()/select() on a socket fd.
static void sock_poll_wake(socket_t* s) {
    if (s && s->file && s->file->poll_waiter) {
        task_t* w = (task_t*)s->file->poll_waiter;
        s->file->poll_waiter = NULL;
        sched_wake(w);
    }
}

// ── VFS operations ────────────────────────────────────────────────────────

static int64_t sock_read(vfs_file_t* self, void* buf, uint64_t len) {
    return (int64_t)socket_recv(self, buf, (uint32_t)len);
}

static int64_t sock_write(vfs_file_t* self, const void* buf, uint64_t len) {
    return (int64_t)socket_send(self, buf, (uint32_t)len);
}

// Non-blocking readiness query.
// Returns 1 if the requested event is ready, 0 otherwise.
// events is a bitmask of POLLIN / POLLOUT / POLLHUP (we answer one bit
// at a time — the syscall layer calls us once per bit it's interested in).
static int sock_poll(vfs_file_t* self, int events) {
    socket_t* s = (socket_t*)self->ctx;
    if (!s) return 0;

    if (s->type == SOCK_STREAM) {
        if (!s->pcb) {
            // Closed PCB → everything reports HUP/ERR, nothing is readable.
            if (events & POLLHUP) return 1;
            if (events & POLLERR) return 1;
            return 0;
        }
        tcp_state_t st = tcp_pcb_state(s->pcb);

        if (events & POLLIN) {
            // Listening socket: readable iff a completed child is waiting.
            if (st == TCP_LISTEN) {
                if (tcp_pcb_has_accept(s->pcb)) return 1;
                return 0;
            }
            // Data available.
            if (tcp_pcb_rx_used(s->pcb) > 0) return 1;
            // Peer FIN → recv() will return 0. POSIX requires this to wake
            // POLLIN so the reader can observe EOF.
            if (tcp_pcb_eof(s->pcb)) return 1;
            // A fully-closed / reset PCB is also readable (returns error).
            if (st == TCP_CLOSED || st == TCP_CLOSE_WAIT) return 1;
        }
        if (events & POLLOUT) {
            // Writable iff established and there's room in the send buffer.
            if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
                if (tcp_pcb_tx_space(s->pcb) > 0) return 1;
            }
        }
        if (events & POLLHUP) {
            // Full hangup: both directions done, or the PCB is CLOSED.
            if (st == TCP_CLOSED || st == TCP_TIME_WAIT) return 1;
            if (tcp_pcb_eof(s->pcb)) return 1;
        }
        if (events & POLLERR) {
            // No distinct error state exposed yet — surface via POLLHUP.
        }
        return 0;
    }

    // UDP socket.
    if (events & POLLIN) {
        if (s->udp_rx_head) return 1;
    }
    if (events & POLLOUT) {
        return 1;  // UDP send is always ready (best-effort).
    }
    return 0;
}

static void sock_close(vfs_file_t* self) {
    socket_t* s = (socket_t*)self->ctx;
    if (!s) { kfree(self); return; }

    if (s->type == SOCK_STREAM && s->pcb) {
        // Detach the poll backpointer first so the TCP layer can't touch
        // a freed vfs_file_t after we let go of it here.
        tcp_pcb_set_file(s->pcb, NULL);
        tcp_close(s->pcb);
        tcp_pcb_free(s->pcb);
        s->pcb = 0;
    }

    if (s->type == SOCK_DGRAM && s->bound) {
        udp_table_remove(s->local_port);
        // Free any pending datagrams.
        skbuff_t* skb = s->udp_rx_head;
        while (skb) {
            skbuff_t* next = skb->next;
            skb_free(skb);
            skb = next;
        }
    }

    kfree(s);
    kfree(self);
}

static int64_t sock_seek(vfs_file_t* self, int64_t offset, int whence) {
    (void)self; (void)offset; (void)whence;
    return -ESPIPE;  // sockets are not seekable
}

// ── socket_open ───────────────────────────────────────────────────────────

vfs_file_t* socket_open(int domain, int type) {
    if (domain != AF_INET) return 0;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return 0;

    socket_t* s = (socket_t*)kmalloc(sizeof(socket_t));
    if (!s) return 0;

    // Zero-initialise.
    uint8_t* p = (uint8_t*)s;
    for (uint32_t i = 0; i < sizeof(socket_t); i++) p[i] = 0;

    s->type = (uint8_t)type;

    if (type == SOCK_STREAM) {
        // Allocate an ephemeral local port for the PCB now; bind() can
        // override it later.
        uint16_t lport = tcp_ephemeral_port();
        s->pcb = tcp_pcb_alloc(lport);
        if (!s->pcb) { kfree(s); return 0; }
        s->local_port = lport;
    }

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) {
        if (s->pcb) tcp_pcb_free(s->pcb);
        kfree(s);
        return 0;
    }

    f->read        = sock_read;
    f->write       = sock_write;
    f->close       = sock_close;
    f->seek        = sock_seek;
    f->poll        = sock_poll;
    f->ioctl       = NULL;
    f->ctx         = s;
    f->poll_waiter = NULL;
    f->flags       = 0;
    f->refcount    = 1;
    f->rights      = 0;
    f->path[0]     = '\0';

    s->file = f;
    if (s->pcb) tcp_pcb_set_file(s->pcb, f);

    return f;
}

// ── socket_bind ───────────────────────────────────────────────────────────

int socket_bind(vfs_file_t* f, uint16_t port) {
    if (!f) return -EBADF;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->bound) return -EINVAL;

    if (s->type == SOCK_DGRAM) {
        int r = udp_table_register(port, s);
        if (r < 0) return r;
        s->local_port = port;
        s->bound = 1;
        return 0;
    }

    // TCP: rebind the PCB to the requested local port.
    if (s->pcb) {
        tcp_pcb_set_file(s->pcb, NULL);
        tcp_pcb_free(s->pcb);
        s->pcb = 0;
    }
    s->pcb = tcp_pcb_alloc(port);
    if (!s->pcb) return -EADDRINUSE;
    tcp_pcb_set_file(s->pcb, f);
    s->local_port = port;
    s->bound = 1;
    return 0;
}

// ── socket_listen ─────────────────────────────────────────────────────────

int socket_listen(vfs_file_t* f) {
    if (!f) return -EBADF;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!s->pcb) return -EINVAL;
    int r = tcp_listen(s->pcb);
    return (r == 0) ? 0 : -EINVAL;
}

// ── socket_accept ─────────────────────────────────────────────────────────

vfs_file_t* socket_accept(vfs_file_t* f, sockaddr_in_t* peer_addr) {
    if (!f) return 0;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_STREAM || !s->pcb) return 0;

    tcp_pcb_t* child_pcb;
    while (!(child_pcb = tcp_accept(s->pcb))) {
        if (f->flags & O_NONBLOCK) {
            // Caller sees NULL; syscall layer must translate to -EAGAIN.
            return 0;
        }
        tcp_pcb_set_waiter(s->pcb, g_current);
        sched_sleep();
    }

    if (peer_addr) {
        // TODO: expose a tcp_pcb_peer() accessor. For now leave zeroed.
        peer_addr->sin_family = AF_INET;
        peer_addr->sin_addr   = 0;
        peer_addr->sin_port   = 0;
    }

    // Wrap the child PCB in a new socket_t / vfs_file_t.
    socket_t* cs = (socket_t*)kmalloc(sizeof(socket_t));
    if (!cs) { tcp_close(child_pcb); tcp_pcb_free(child_pcb); return 0; }
    uint8_t* p = (uint8_t*)cs;
    for (uint32_t i = 0; i < sizeof(socket_t); i++) p[i] = 0;
    cs->type  = SOCK_STREAM;
    cs->pcb   = child_pcb;
    cs->bound = 1;

    vfs_file_t* cf = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!cf) { kfree(cs); tcp_close(child_pcb); tcp_pcb_free(child_pcb); return 0; }
    cf->read        = sock_read;
    cf->write       = sock_write;
    cf->close       = sock_close;
    cf->seek        = sock_seek;
    cf->poll        = sock_poll;
    cf->ioctl       = NULL;
    cf->ctx         = cs;
    cf->poll_waiter = NULL;
    cf->flags       = 0;
    cf->refcount    = 1;
    cf->rights      = 0;
    cf->path[0]     = '\0';

    cs->file = cf;
    tcp_pcb_set_file(child_pcb, cf);
    return cf;
}

// ── socket_connect ────────────────────────────────────────────────────────

int socket_connect(vfs_file_t* f, uint32_t dst_ip_be, uint16_t dst_port) {
    if (!f) return -EBADF;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;

    if (s->type == SOCK_DGRAM) {
        // Connected UDP: stash peer for subsequent send()/recv().
        s->udp_peer_ip   = dst_ip_be;
        s->udp_peer_port = dst_port;
        return 0;
    }
    if (s->type != SOCK_STREAM || !s->pcb) return -EINVAL;

    tcp_state_t st = tcp_pcb_state(s->pcb);
    if (st == TCP_ESTABLISHED) return -EISCONN;

    if (st == TCP_CLOSED) {
        int r = tcp_connect(s->pcb, dst_ip_be, dst_port);
        if (r < 0) return r;  // propagate tcp_connect's errno
    }

    // Non-blocking connect: caller polls for POLLOUT.
    if (f->flags & O_NONBLOCK) {
        tcp_state_t now = tcp_pcb_state(s->pcb);
        if (now == TCP_ESTABLISHED) return 0;
        if (now == TCP_SYN_SENT || now == TCP_SYN_RCVD) return -EINPROGRESS;
        return -ECONNREFUSED;
    }

    // Block until ESTABLISHED or the connection fails (RST/timeout).
    while (tcp_pcb_state(s->pcb) == TCP_SYN_SENT ||
           tcp_pcb_state(s->pcb) == TCP_SYN_RCVD) {
        tcp_pcb_set_waiter(s->pcb, g_current);
        sched_sleep();
    }

    tcp_state_t final = tcp_pcb_state(s->pcb);
    if (final == TCP_ESTABLISHED) return 0;
    if (final == TCP_CLOSED)      return -ECONNREFUSED;
    return -ECONNABORTED;
}

// ── socket_send ───────────────────────────────────────────────────────────

int socket_send(vfs_file_t* f, const void* buf, uint32_t len) {
    if (!f) return -EBADF;
    if (!buf || !len) return -EINVAL;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;

    int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

    if (s->type == SOCK_STREAM) {
        if (!s->pcb) return -ENOTCONN;
        return tcp_send(s->pcb, buf, len, nonblock);
    }

    // UDP: requires a peer set via connect() or sendto().
    if (s->udp_peer_ip == 0 || s->udp_peer_port == 0) return -EDESTADDRREQ;
    int r = udp_send(s->udp_peer_ip, s->local_port, s->udp_peer_port,
                      buf, (uint16_t)len);
    return (r < 0) ? -EIO : (int)len;
}

// ── socket_recv ───────────────────────────────────────────────────────────

int socket_recv(vfs_file_t* f, void* buf, uint32_t len) {
    if (!f) return -EBADF;
    if (!buf || !len) return -EINVAL;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;

    int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

    if (s->type == SOCK_STREAM) {
        if (!s->pcb) return -ENOTCONN;
        return tcp_recv_data(s->pcb, buf, len, nonblock);
    }

    // UDP: wait for a datagram (or EAGAIN in nonblock).
    while (!s->udp_rx_head) {
        if (nonblock) return -EAGAIN;
        s->waiter = g_current;
        sched_sleep();
        s->waiter = 0;
    }

    skbuff_t* skb = s->udp_rx_head;
    s->udp_rx_head = skb->next;
    if (!s->udp_rx_head) s->udp_rx_tail = 0;
    s->udp_rx_count--;

    uint32_t copy = skb->len < len ? skb->len : len;
    uint8_t* src  = (uint8_t*)skb->data;
    uint8_t* dst  = (uint8_t*)buf;
    for (uint32_t i = 0; i < copy; i++) dst[i] = src[i];
    skb_free(skb);
    return (int)copy;
}

// ── socket_sendto ─────────────────────────────────────────────────────────

// Detect broadcast destinations (limited broadcast or the subnet broadcast).
static int is_broadcast_dst(uint32_t dst_ip_be) {
    if (dst_ip_be == 0xFFFFFFFFu) return 1;
    uint32_t bcast = net_broadcast_ip();
    if (bcast != 0 && dst_ip_be == bcast) return 1;
    return 0;
}

int socket_sendto(vfs_file_t* f, const void* buf, uint32_t len,
                   uint32_t dst_ip_be, uint16_t dst_port) {
    if (!f) return -EBADF;
    if (!buf || !len) return -EINVAL;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -EOPNOTSUPP;

    // POSIX: sending to a broadcast address requires SO_BROADCAST.
    if (is_broadcast_dst(dst_ip_be) && !s->broadcast) return -EACCES;

    // Remember peer for subsequent send() calls.
    s->udp_peer_ip   = dst_ip_be;
    s->udp_peer_port = dst_port;

    // DHCP: when no local IP has been assigned, send from 0.0.0.0 so the
    // server sees the correct ciaddr in the DHCP header.
    uint32_t src_ip = net_our_ip();
    int r = udp_send_ex(src_ip, dst_ip_be, s->local_port, dst_port,
                         buf, (uint16_t)len);
    return (r < 0) ? -EIO : (int)len;
}

// ── socket_recvfrom ───────────────────────────────────────────────────────

int socket_recvfrom(vfs_file_t* f, void* buf, uint32_t len,
                     sockaddr_in_t* src_addr) {
    if (!f) return -EBADF;
    if (!buf || !len) return -EINVAL;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -EOPNOTSUPP;

    int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

    while (!s->udp_rx_head) {
        if (nonblock) return -EAGAIN;
        s->waiter = g_current;
        sched_sleep();
        s->waiter = 0;
    }

    skbuff_t* skb = s->udp_rx_head;
    s->udp_rx_head = skb->next;
    if (!s->udp_rx_head) s->udp_rx_tail = 0;
    s->udp_rx_count--;

    if (src_addr) {
        src_addr->sin_family = AF_INET;
        src_addr->sin_addr   = skb->src_ip_be;
        src_addr->sin_port   = skb->src_port_be;  // network byte order
    }

    uint32_t copy = skb->len < len ? skb->len : len;
    uint8_t* src  = (uint8_t*)skb->data;
    uint8_t* dst  = (uint8_t*)buf;
    for (uint32_t i = 0; i < copy; i++) dst[i] = src[i];
    skb_free(skb);
    return (int)copy;
}

// ── socket_shutdown ───────────────────────────────────────────────────────

int socket_shutdown(vfs_file_t* f, int how) {
    if (!f) return -EBADF;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM || !s->pcb) return -ENOTCONN;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;
    if (how == SHUT_WR || how == SHUT_RDWR)
        tcp_close(s->pcb);
    return 0;
}

// ── socket_setsockopt ─────────────────────────────────────────────────────
// We honour the options that user-space actually needs.  Timeouts are
// accepted but not yet enforced (best-effort — documented in the code).

int socket_setsockopt(vfs_file_t* f, int level, int optname,
                       const void* optval, uint32_t optlen) {
    if (!f) return -EBADF;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -EBADF;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (!optval || optlen == 0) return -EINVAL;

    switch (optname) {
    case SO_BROADCAST: {
        if (optlen < sizeof(int)) return -EINVAL;
        int v = *(const int*)optval;
        s->broadcast = v ? 1 : 0;
        return 0;
    }
    case SO_REUSEADDR:
    case SO_REUSEPORT:
        // We do not currently share ports between sockets, but accept the
        // request silently so ordinary servers (which set it unconditionally)
        // keep working.
        return 0;
    case SO_RCVBUF:
    case SO_SNDBUF:
        // Buffer sizes are fixed at compile time (see TCP_RXBUF_SIZE etc).
        return 0;
    case SO_KEEPALIVE:
        // TCP keepalive not implemented yet — accept silently.
        return 0;
    case SO_RCVTIMEO:
    case SO_SNDTIMEO:
        // Timeouts accepted but not yet enforced. Non-blocking callers
        // should set O_NONBLOCK via fcntl(F_SETFL) instead.
        return 0;
    case SO_LINGER:
        return 0;
    case SO_ERROR:
        // SO_ERROR is getsockopt-only.
        return -EINVAL;
    default:
        return -ENOPROTOOPT;
    }
}

// ── socket_deliver_udp ────────────────────────────────────────────────────
// Called from udp_recv() with dst_port in host byte order.
// Enqueues the skb into the matching UDP socket's receive queue.

void socket_deliver_udp(uint16_t dst_port, skbuff_t* skb) {
    socket_t* s = udp_table_find(dst_port);
    if (!s) { skb_free(skb); return; }

    // Drop if queue is full — upper bound avoids unbounded memory growth.
    if (s->udp_rx_count >= UDP_RX_QUEUE_MAX) { skb_free(skb); return; }

    // Enqueue. The skb already has src_ip_be and src_port_be stamped.
    skb->next = 0;
    if (s->udp_rx_tail) {
        s->udp_rx_tail->next = skb;
    } else {
        s->udp_rx_head = skb;
    }
    s->udp_rx_tail = skb;
    s->udp_rx_count++;

    // Wake any task blocked in socket_recv / socket_recvfrom.
    if (s->waiter) {
        task_t* w = (task_t*)s->waiter;
        s->waiter = 0;
        sched_wake(w);
    }
    // And any task sleeping in poll()/select() on this fd.
    sock_poll_wake(s);
}
