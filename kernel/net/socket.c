#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "skbuff.h"
#include "common.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"

// ── UDP socket registry ───────────────────────────────────────────────────
// Simple fixed-size table: maps local port → socket_t*.
// Only populated while a UDP socket is open and bound.

#define UDP_SOCK_TABLE_SIZE  64u

typedef struct {
    uint16_t  port;   // host byte order; 0 = empty slot
    socket_t* sock;
} udp_sock_entry_t;

static udp_sock_entry_t s_udp_table[UDP_SOCK_TABLE_SIZE];

static void udp_table_register(uint16_t port, socket_t* s) {
    for (uint32_t i = 0; i < UDP_SOCK_TABLE_SIZE; i++) {
        if (s_udp_table[i].port == 0) {
            s_udp_table[i].port = port;
            s_udp_table[i].sock = s;
            return;
        }
    }
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

// ── VFS operations ────────────────────────────────────────────────────────

static int64_t sock_read(vfs_file_t* self, void* buf, uint64_t len) {
    return (int64_t)socket_recv(self, buf, (uint32_t)len);
}

static int64_t sock_write(vfs_file_t* self, const void* buf, uint64_t len) {
    return (int64_t)socket_send(self, buf, (uint32_t)len);
}

static void sock_close(vfs_file_t* self) {
    socket_t* s = (socket_t*)self->ctx;
    if (!s) { kfree(self); return; }

    if (s->type == SOCK_STREAM && s->pcb) {
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
    return -1;  // sockets are not seekable
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

    f->read     = sock_read;
    f->write    = sock_write;
    f->close       = sock_close;
    f->seek        = sock_seek;
    f->poll        = NULL;
    f->ioctl       = NULL;
    f->ctx         = s;
    f->poll_waiter = NULL;
    f->flags       = 0;
    f->refcount    = 1;
    f->rights   = 0;
    f->path[0]  = '\0';

    return f;
}

// ── socket_bind ───────────────────────────────────────────────────────────

int socket_bind(vfs_file_t* f, uint16_t port) {
    if (!f) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->bound) return -1;

    s->local_port = port;
    s->bound = 1;

    if (s->type == SOCK_DGRAM) {
        udp_table_register(port, s);
    }
    // For TCP the PCB was already allocated with an ephemeral port; we
    // re-allocate with the desired port.
    if (s->type == SOCK_STREAM && s->pcb) {
        tcp_pcb_free(s->pcb);
        s->pcb = tcp_pcb_alloc(port);
        if (!s->pcb) return -1;
    }

    return 0;
}

// ── socket_listen ─────────────────────────────────────────────────────────

int socket_listen(vfs_file_t* f) {
    if (!f) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_STREAM || !s->pcb) return -1;
    return tcp_listen(s->pcb);
}

// ── socket_accept ─────────────────────────────────────────────────────────

vfs_file_t* socket_accept(vfs_file_t* f, sockaddr_in_t* peer_addr) {
    if (!f) return 0;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_STREAM || !s->pcb) return 0;

    // Block until a completed connection is in the accept queue.
    // tcp_recv() calls pcb_wake(listener) when a child enters ESTABLISHED.
    tcp_pcb_t* child_pcb;
    while (!(child_pcb = tcp_accept(s->pcb))) {
        tcp_pcb_set_waiter(s->pcb, g_current);
        sched_sleep();
    }

    // Optionally fill peer address.
    if (peer_addr) {
        // We don't have a clean accessor yet; leave zeroed for now.
        // TODO: tcp_pcb_peer(child_pcb, &peer_addr->sin_addr, &peer_addr->sin_port)
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
    cf->read     = sock_read;
    cf->write    = sock_write;
    cf->close    = sock_close;
    cf->seek     = sock_seek;
    cf->poll        = NULL;
    cf->ioctl       = NULL;
    cf->ctx         = cs;
    cf->poll_waiter = NULL;
    cf->flags       = 0;
    cf->refcount    = 1;
    cf->rights   = 0;
    cf->path[0]  = '\0';
    return cf;
}

// ── socket_connect ────────────────────────────────────────────────────────

int socket_connect(vfs_file_t* f, uint32_t dst_ip_be, uint16_t dst_port) {
    if (!f) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_STREAM || !s->pcb) return -1;

    int r = tcp_connect(s->pcb, dst_ip_be, dst_port);
    if (r != 0) return -1;

    // Block until ESTABLISHED or the connection fails (RST/timeout).
    // tcp_recv() calls pcb_wake() when the state changes out of SYN_SENT.
    while (tcp_pcb_state(s->pcb) == TCP_SYN_SENT) {
        tcp_pcb_set_waiter(s->pcb, g_current);
        sched_sleep();
    }

    return (tcp_pcb_state(s->pcb) == TCP_ESTABLISHED) ? 0 : -1;
}

// ── socket_send ───────────────────────────────────────────────────────────

int socket_send(vfs_file_t* f, const void* buf, uint32_t len) {
    if (!f || !len) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -1;

    if (s->type == SOCK_STREAM) {
        if (!s->pcb) return -1;
        return tcp_send(s->pcb, buf, len);
    }

    // UDP: requires a peer set via connect() or sendto().
    if (s->udp_peer_ip == 0 || s->udp_peer_port == 0) return -1;
    return udp_send(s->udp_peer_ip, s->local_port, s->udp_peer_port, buf, (uint16_t)len);
}

// ── socket_recv ───────────────────────────────────────────────────────────

int socket_recv(vfs_file_t* f, void* buf, uint32_t len) {
    if (!f || !len) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s) return -1;

    if (s->type == SOCK_STREAM) {
        if (!s->pcb) return -1;
        return tcp_recv_data(s->pcb, buf, len);
    }

    // UDP: block until a datagram arrives.
    while (!s->udp_rx_head) {
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

int socket_sendto(vfs_file_t* f, const void* buf, uint32_t len,
                   uint32_t dst_ip_be, uint16_t dst_port) {
    if (!f || !len) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_DGRAM) return -1;

    // Remember peer for subsequent send() calls.
    s->udp_peer_ip   = dst_ip_be;
    s->udp_peer_port = dst_port;

    return udp_send(dst_ip_be, s->local_port, dst_port, buf, (uint16_t)len);
}

// ── socket_recvfrom ───────────────────────────────────────────────────────

int socket_recvfrom(vfs_file_t* f, void* buf, uint32_t len,
                     sockaddr_in_t* src_addr) {
    if (!f || !len) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_DGRAM) return -1;

    while (!s->udp_rx_head) {
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
        // src port is in the UDP header before it was stripped;
        // we stash it in udp_peer_port when delivering.
        src_addr->sin_port   = s->udp_peer_port;
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
    if (!f) return -1;
    socket_t* s = (socket_t*)f->ctx;
    if (!s || s->type != SOCK_STREAM || !s->pcb) return -1;
    if (how == SHUT_WR || how == SHUT_RDWR)
        tcp_close(s->pcb);
    return 0;
}

// ── socket_deliver_udp ────────────────────────────────────────────────────
// Called from udp_recv() with dst_port in host byte order.
// Enqueues the skb into the matching UDP socket's receive queue.

void socket_deliver_udp(uint16_t dst_port, skbuff_t* skb) {
    socket_t* s = udp_table_find(dst_port);
    if (!s) { skb_free(skb); return; }

    // Remember sender port from the UDP header (already pulled; use trans_hdr).
    // udp_recv() sets skb->trans_hdr before calling us.
    if (skb->trans_hdr) {
        // The udp_hdr_t is at trans_hdr; src_port is first field.
        uint16_t sp = ((uint16_t*)skb->trans_hdr)[0];
        // Convert from network byte order.
        s->udp_peer_port = (uint16_t)((sp >> 8) | (sp << 8));
    }
    s->udp_peer_ip = skb->src_ip_be;

    // Enqueue.
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
}
