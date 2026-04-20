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
#include "smp.h"
#include "rcu.h"

// ── UDP socket registry (RCU-protected) ──────────────────────────────────
// The whole table is a single object (udp_table_t) published via
// rcu_assign_pointer.  Readers (udp_table_find, called from the NIC
// softirq path per packet) walk lock-free inside rcu_read_lock() — zero
// atomics, zero cache-line bouncing.  Writers (register/remove) serialise
// on s_udp_wlock, build a fresh table via copy-on-write, and call_rcu the
// old one for deferred free.  Mutation is O(cap) but register/remove are
// once-per-socket events, while find is per-packet.

#define UDP_RX_QUEUE_MAX     64u   // per-socket cap on pending datagrams
#define UDP_HT_INIT_CAP      32u   // initial power-of-2 slot count

typedef struct {
    uint16_t  port;   // host byte order; 0 = empty
    socket_t* sock;
} udp_sock_entry_t;

typedef struct udp_table {
    uint32_t         cap;
    uint32_t         cnt;
    rcu_head_t       rcu_head;   // Phase 5B: embedded for call_rcu_head
    udp_sock_entry_t slots[];
} udp_table_t;

static udp_table_t* s_udp       = NULL;   // RCU-protected
static spinlock_t   s_udp_wlock = SPINLOCK_INIT;

static uint32_t udp_hash(uint16_t port, uint32_t cap) {
    return ((uint32_t)port * 2654435761u) & (cap - 1u);
}

static udp_table_t* udp_table_alloc(uint32_t cap) {
    udp_table_t* t = (udp_table_t*)kmalloc(sizeof(udp_table_t) +
                                            (uint64_t)cap * sizeof(udp_sock_entry_t));
    if (!t) return NULL;
    t->cap = cap;
    t->cnt = 0;
    __builtin_memset(t->slots, 0, (uint64_t)cap * sizeof(udp_sock_entry_t));
    return t;
}

static void udp_table_raw_insert(udp_table_t* t, uint16_t port, socket_t* s) {
    uint32_t i = udp_hash(port, t->cap);
    while (t->slots[i].port) i = (i + 1u) & (t->cap - 1u);
    t->slots[i].port = port;
    t->slots[i].sock = s;
    t->cnt++;
}

static void udp_table_free_rcu(void* p) { kfree(p); }

static int udp_table_register(uint16_t port, socket_t* s) {
    uint64_t flags = spin_lock_irqsave(&s_udp_wlock);
    udp_table_t* old = s_udp;
    uint32_t old_cap = old ? old->cap : 0;
    uint32_t old_cnt = old ? old->cnt : 0;

    // Duplicate check.
    if (old) {
        uint32_t i = udp_hash(port, old_cap);
        for (uint32_t n = 0; n < old_cap; n++) {
            if (!old->slots[i].port) break;
            if (old->slots[i].port == port) {
                spin_unlock_irqrestore(&s_udp_wlock, flags);
                return -EADDRINUSE;
            }
            i = (i + 1u) & (old_cap - 1u);
        }
    }

    uint32_t new_cap = old_cap ? old_cap : UDP_HT_INIT_CAP;
    if ((old_cnt + 1u) * 4u >= new_cap * 3u)
        new_cap = old_cap ? old_cap * 2u : UDP_HT_INIT_CAP;

    udp_table_t* neu = udp_table_alloc(new_cap);
    if (!neu) { spin_unlock_irqrestore(&s_udp_wlock, flags); return -ENOMEM; }

    if (old) {
        for (uint32_t i = 0; i < old->cap; i++)
            if (old->slots[i].port)
                udp_table_raw_insert(neu, old->slots[i].port, old->slots[i].sock);
    }
    udp_table_raw_insert(neu, port, s);

    rcu_assign_pointer(s_udp, neu);
    spin_unlock_irqrestore(&s_udp_wlock, flags);
    if (old) call_rcu_head(&old->rcu_head, udp_table_free_rcu, old);
    return 0;
}

static void udp_table_remove(uint16_t port) {
    uint64_t flags = spin_lock_irqsave(&s_udp_wlock);
    udp_table_t* old = s_udp;
    if (!old) { spin_unlock_irqrestore(&s_udp_wlock, flags); return; }

    udp_table_t* neu = udp_table_alloc(old->cap);
    if (!neu) { spin_unlock_irqrestore(&s_udp_wlock, flags); return; }
    int removed = 0;
    for (uint32_t i = 0; i < old->cap; i++) {
        if (!old->slots[i].port) continue;
        if (!removed && old->slots[i].port == port) { removed = 1; continue; }
        udp_table_raw_insert(neu, old->slots[i].port, old->slots[i].sock);
    }

    rcu_assign_pointer(s_udp, neu);
    spin_unlock_irqrestore(&s_udp_wlock, flags);
    call_rcu_head(&old->rcu_head, udp_table_free_rcu, old);
}

// Reader — must be inside rcu_read_lock().
static socket_t* udp_table_find(uint16_t port) {
    udp_table_t* t = rcu_dereference(s_udp);
    if (!t) return NULL;
    uint32_t cap = t->cap;
    uint32_t i = udp_hash(port, cap);
    for (uint32_t n = 0; n < cap; n++) {
        if (!t->slots[i].port) return NULL;
        if (t->slots[i].port == port) return t->slots[i].sock;
        i = (i + 1u) & (cap - 1u);
    }
    return NULL;
}

// Wake all tasks sitting in poll()/epoll_wait() on a socket fd.
static void sock_poll_wake(socket_t* s) {
    if (s && s->file) wait_queue_wake_all(s->file->waitq);
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

// Deferred socket free — runs after an RCU grace period so any in-flight
// udp_table_find / socket_deliver_udp reader that observed this socket
// has dropped out of its reader section.
static void sock_free_rcu(void* data) {
    socket_t* s = (socket_t*)data;
    if (s->type == SOCK_DGRAM) {
        skbuff_t* skb = s->udp_rx_head;
        while (skb) {
            skbuff_t* next = skb->next;
            skb_free(skb);
            skb = next;
        }
    }
    kfree(s);
}

static void sock_close(vfs_file_t* self) {
    socket_t* s = (socket_t*)self->ctx;
    if (!s) { kfree(self); return; }

    if (s->type == SOCK_STREAM && s->pcb) {
        // Detach the poll backpointer first so the TCP layer can't touch
        // a freed vfs_file_t after we let go of it here.
        tcp_pcb_set_file(s->pcb, NULL);
        tcp_close(s->pcb);
        tcp_pcb_free(s->pcb);  // call_rcu-deferred inside tcp_pcb_free
        s->pcb = 0;
    }

    // Unpublish from the UDP registry first; readers that already observed
    // the socket are still safe because the final free is deferred below.
    if (s->type == SOCK_DGRAM && s->bound)
        udp_table_remove(s->local_port);

    // Expedited: this path is close() on an inet socket — user-syscall
    // return blocks on the RCU grace period without it.
    call_rcu_expedited(sock_free_rcu, s);
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

    __builtin_memset(s, 0, sizeof(socket_t));
    wait_queue_init(&s->waitq);

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
    f->poll           = sock_poll;
    f->ioctl          = NULL;
    f->ctx            = s;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount       = 1;
    f->rights         = 0;
    f->path[0]        = '\0';

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
    __builtin_memset(cs, 0, sizeof(socket_t));
    wait_queue_init(&cs->waitq);
    cs->type  = SOCK_STREAM;
    cs->pcb   = child_pcb;
    cs->bound = 1;

    vfs_file_t* cf = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!cf) { kfree(cs); tcp_close(child_pcb); tcp_pcb_free(child_pcb); return 0; }
    cf->read        = sock_read;
    cf->write       = sock_write;
    cf->close       = sock_close;
    cf->seek        = sock_seek;
    cf->poll           = sock_poll;
    cf->ioctl          = NULL;
    cf->ctx            = cs;
    cf->waitq           = &cf->_waitq; wait_queue_init(cf->waitq);
    cf->secondary_waitq = NULL;
    cf->flags          = 0;
    cf->refcount       = 1;
    cf->rights         = 0;
    cf->path[0]        = '\0';

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

static int udp_auto_bind(socket_t* s);

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
    int ar = udp_auto_bind(s);
    if (ar < 0) return ar;
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
    if (!s->udp_rx_head) {
        if (nonblock) return -EAGAIN;
        WAIT_EVENT(&s->waitq, s->udp_rx_head != NULL);
    }

    skbuff_t* skb = s->udp_rx_head;
    s->udp_rx_head = skb->next;
    if (!s->udp_rx_head) s->udp_rx_tail = 0;
    s->udp_rx_count--;

    uint32_t copy = skb->len < len ? skb->len : len;
    __builtin_memcpy(buf, skb->data, copy);
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

// Auto-allocate and register an ephemeral UDP port if the socket hasn't
// been explicitly bound.  Required so datagram replies can be demuxed back
// to this socket.  Starts at 49152; retries on collision.
static int udp_auto_bind(socket_t* s) {
    if (s->bound && s->local_port) return 0;
    for (int attempt = 0; attempt < 64; attempt++) {
        uint16_t p = tcp_ephemeral_port();           // shared 49152-65535 pool
        if (p < 1024) continue;
        int r = udp_table_register(p, s);
        if (r == 0) {
            s->local_port = p;
            s->bound      = 1;
            return 0;
        }
        if (r != -EADDRINUSE) return r;
    }
    return -EADDRINUSE;
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

    // Auto-bind to an ephemeral port so the reply can be demuxed back.
    int ar = udp_auto_bind(s);
    if (ar < 0) return ar;

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

    if (!s->udp_rx_head) {
        if (nonblock) return -EAGAIN;
        WAIT_EVENT(&s->waitq, s->udp_rx_head != NULL);
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
    __builtin_memcpy(buf, skb->data, copy);
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
    // Reader section covers both the table lookup and the enqueue onto
    // the socket's RX ring.  If a concurrent sock_close() on `s` fires,
    // the final teardown is deferred via call_rcu so `s` stays valid
    // until we rcu_read_unlock.  Nothing in this section sleeps.
    rcu_read_lock();
    socket_t* s = udp_table_find(dst_port);
    if (!s) { rcu_read_unlock(); skb_free(skb); return; }

    // Drop if queue is full — upper bound avoids unbounded memory growth.
    if (s->udp_rx_count >= UDP_RX_QUEUE_MAX) {
        rcu_read_unlock();
        skb_free(skb);
        return;
    }

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
    wait_queue_wake_all(&s->waitq);
    // And any task sleeping in poll()/select() on this fd.
    sock_poll_wake(s);
    rcu_read_unlock();
}
