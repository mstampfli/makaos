#include "unix_sock.h"
#include "kheap.h"
#include "errno.h"
#include "sched.h"
#include "process.h"

// ── Bind namespace ───────────────────────────────────────────────────────
// Flat table mapping path → unix_sock_t*.  Protected by the single-threaded
// kernel (add spinlock when SMP arrives).

typedef struct {
    char         path[UNIX_PATH_MAX];
    unix_sock_t* sock;
} unix_ns_entry_t;

static unix_ns_entry_t s_unix_ns[UNIX_NS_MAX];

static int ns_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static unix_sock_t* ns_find(const char* path) {
    for (int i = 0; i < UNIX_NS_MAX; i++) {
        if (s_unix_ns[i].sock && ns_streq(s_unix_ns[i].path, path))
            return s_unix_ns[i].sock;
    }
    return NULL;
}

static int ns_insert(const char* path, unix_sock_t* sock) {
    if (ns_find(path)) return -EADDRINUSE;
    for (int i = 0; i < UNIX_NS_MAX; i++) {
        if (!s_unix_ns[i].sock) {
            int j = 0;
            while (path[j] && j < UNIX_PATH_MAX - 1) {
                s_unix_ns[i].path[j] = path[j];
                j++;
            }
            s_unix_ns[i].path[j] = '\0';
            s_unix_ns[i].sock = sock;
            return 0;
        }
    }
    return -ENOSPC;
}

static void ns_remove(const char* path) {
    for (int i = 0; i < UNIX_NS_MAX; i++) {
        if (s_unix_ns[i].sock && ns_streq(s_unix_ns[i].path, path)) {
            s_unix_ns[i].sock = NULL;
            s_unix_ns[i].path[0] = '\0';
            return;
        }
    }
}

// ── Internal helpers ─────────────────────────────────────────────────────

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void zero_mem(void* p, uint32_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

// Wake a task sleeping on a socket.
static void unix_wake(unix_sock_t* s) {
    if (s->waiter) {
        task_t* w = (task_t*)s->waiter;
        s->waiter = NULL;
        sched_wake(w);
    }
}

// ── Circular buffer operations (SOCK_STREAM) ────────────────────────────

static uint32_t cbuf_write(unix_sock_t* s, const void* data, uint32_t len) {
    uint32_t avail = UNIX_BUF_SIZE - s->buf_count;
    if (len > avail) len = avail;
    const uint8_t* src = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) {
        s->buf[s->buf_tail] = src[i];
        s->buf_tail = (s->buf_tail + 1) & (UNIX_BUF_SIZE - 1);
    }
    s->buf_count += len;
    return len;
}

static uint32_t cbuf_read(unix_sock_t* s, void* data, uint32_t len) {
    if (len > s->buf_count) len = s->buf_count;
    uint8_t* dst = (uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = s->buf[s->buf_head];
        s->buf_head = (s->buf_head + 1) & (UNIX_BUF_SIZE - 1);
    }
    s->buf_count -= len;
    return len;
}

// ── VFS callbacks ────────────────────────────────────────────────────────

static int64_t unix_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    return (int64_t)unix_sock_recv(self, buf, (uint32_t)len);
}

static int64_t unix_vfs_write(vfs_file_t* self, const void* buf, uint64_t len) {
    return (int64_t)unix_sock_send(self, buf, (uint32_t)len);
}

static int unix_vfs_poll(vfs_file_t* self, int events) {
    unix_sock_t* s = (unix_sock_t*)self->ctx;
    if (!s) return 0;

    if (events & 0x0001 /* POLLIN */) {
        // Readable if: stream has data, dgram has messages, or peer disconnected.
        if (s->type == SOCK_STREAM) {
            if (s->buf_count > 0) return 1;
            if (s->state == UNIX_STATE_DISCONNECTED) return 1; // EOF
        } else {
            if (s->dgram_head) return 1;
        }
        // Listening socket: readable if pending connections.
        if (s->state == UNIX_STATE_LISTENING && s->backlog_head) return 1;
        // Ancillary data waiting.
        if (s->ancillary.count > 0) return 1;
    }
    if (events & 0x0004 /* POLLOUT */) {
        if (s->type == SOCK_STREAM && s->peer) {
            if (s->peer->buf_count < UNIX_BUF_SIZE) return 1;
        }
        if (s->type == SOCK_DGRAM) return 1; // dgram always writable
    }
    return 0;
}

void unix_sock_close(vfs_file_t* self) {
    unix_sock_t* s = (unix_sock_t*)self->ctx;
    if (!s) { kfree(self); return; }

    // Remove from namespace if bound.
    if (s->path[0])
        ns_remove(s->path);

    // Notify peer of disconnection.
    if (s->peer) {
        s->peer->peer = NULL;
        s->peer->state = UNIX_STATE_DISCONNECTED;
        unix_wake(s->peer);
    }

    // Free pending connections in backlog.
    unix_pending_t* p = s->backlog_head;
    while (p) {
        unix_pending_t* next = p->next;
        // Wake the client that was waiting for accept — it'll see ECONNREFUSED.
        if (p->client) {
            p->client->state = UNIX_STATE_UNCONNECTED;
            unix_wake(p->client);
        }
        kfree(p);
        p = next;
    }

    // Free datagram queue.
    unix_dgram_t* d = s->dgram_head;
    while (d) {
        unix_dgram_t* next = d->next;
        kfree(d);
        d = next;
    }

    // Free ancillary (in-flight fds that were never received).
    for (uint8_t i = 0; i < s->ancillary.count; i++) {
        uint8_t idx = (s->ancillary.head + i) % UNIX_ANCILLARY_MAX;
        if (s->ancillary.files[idx])
            vfs_close(s->ancillary.files[idx]);
    }

    kfree(s);
    kfree(self);
}

// ── unix_sock_open ───────────────────────────────────────────────────────

vfs_file_t* unix_sock_open(int type) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return NULL;

    unix_sock_t* s = kmalloc(sizeof(unix_sock_t));
    if (!s) return NULL;
    zero_mem(s, sizeof(unix_sock_t));

    s->type  = (uint8_t)type;
    s->state = UNIX_STATE_UNCONNECTED;

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(s); return NULL; }

    f->read     = unix_vfs_read;
    f->write    = unix_vfs_write;
    f->close    = unix_sock_close;
    f->seek     = NULL;
    f->poll     = unix_vfs_poll;
    f->ctx      = s;
    f->flags    = 0;
    f->refcount = 1;
    f->rights   = 0xFFFFFFFF;
    f->path[0]  = '\0';

    return f;
}

// ── unix_sock_bind ───────────────────────────────────────────────────────

int unix_sock_bind(vfs_file_t* f, const char* path) {
    if (!f || !path || !path[0]) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->path[0]) return -EINVAL; // already bound

    int rc = ns_insert(path, s);
    if (rc) return rc;

    str_copy(s->path, path, UNIX_PATH_MAX);
    if (s->state == UNIX_STATE_UNCONNECTED)
        s->state = UNIX_STATE_BOUND;
    return 0;
}

// ── unix_sock_listen ─────────────────────────────────────────────────────

int unix_sock_listen(vfs_file_t* f, int backlog) {
    if (!f) return -EBADF;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->state != UNIX_STATE_BOUND) return -EINVAL; // must bind first

    s->state = UNIX_STATE_LISTENING;
    s->backlog_max = (uint32_t)backlog;
    if (s->backlog_max == 0) s->backlog_max = 1;
    if (s->backlog_max > UNIX_BACKLOG_MAX) s->backlog_max = UNIX_BACKLOG_MAX;
    return 0;
}

// ── unix_sock_accept ─────────────────────────────────────────────────────

vfs_file_t* unix_sock_accept(vfs_file_t* f) {
    if (!f) return NULL;
    unix_sock_t* listener = (unix_sock_t*)f->ctx;
    if (!listener || listener->type != SOCK_STREAM) return NULL;
    if (listener->state != UNIX_STATE_LISTENING) return NULL;

    // Block until a pending connection is available.
    while (!listener->backlog_head) {
        listener->waiter = g_current;
        sched_sleep();
        // If listener was closed while we slept, bail out.
        if (listener->state != UNIX_STATE_LISTENING) return NULL;
    }

    // Dequeue the first pending connection.
    unix_pending_t* pend = listener->backlog_head;
    listener->backlog_head = pend->next;
    if (!listener->backlog_head) listener->backlog_tail = NULL;
    listener->backlog_count--;

    unix_sock_t* client = pend->client;
    kfree(pend);

    if (!client) return NULL;

    // Create the server-side socket that pairs with the client.
    vfs_file_t* server_f = unix_sock_open(SOCK_STREAM);
    if (!server_f) {
        // Reject the client.
        client->state = UNIX_STATE_UNCONNECTED;
        unix_wake(client);
        return NULL;
    }

    unix_sock_t* server = (unix_sock_t*)server_f->ctx;

    // Link the pair.
    server->peer  = client;
    client->peer  = server;
    server->state = UNIX_STATE_CONNECTED;
    client->state = UNIX_STATE_CONNECTED;

    // Wake the client blocked in connect().
    unix_wake(client);

    return server_f;
}

// ── unix_sock_connect ────────────────────────────────────────────────────

int unix_sock_connect(vfs_file_t* f, const char* path) {
    if (!f || !path || !path[0]) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;

    unix_sock_t* target = ns_find(path);
    if (!target) return -ECONNREFUSED;

    if (s->type == SOCK_DGRAM) {
        // SOCK_DGRAM: just remember the default destination.
        s->peer = target;
        s->state = UNIX_STATE_CONNECTED;
        return 0;
    }

    // SOCK_STREAM: queue ourselves on the listener's backlog.
    if (s->state != UNIX_STATE_UNCONNECTED && s->state != UNIX_STATE_BOUND)
        return -EISCONN;
    if (target->type != SOCK_STREAM) return -ECONNREFUSED;
    if (target->state != UNIX_STATE_LISTENING) return -ECONNREFUSED;
    if (target->backlog_count >= target->backlog_max) return -ECONNREFUSED;

    // Enqueue ourselves.
    unix_pending_t* pend = kmalloc(sizeof(unix_pending_t));
    if (!pend) return -ENOMEM;
    pend->next   = NULL;
    pend->client = s;

    if (target->backlog_tail) {
        target->backlog_tail->next = pend;
    } else {
        target->backlog_head = pend;
    }
    target->backlog_tail = pend;
    target->backlog_count++;

    s->state = UNIX_STATE_CONNECTING;

    // Wake the listener if it's blocked in accept().
    unix_wake(target);

    // Block until accept() completes the pairing (or listener closes).
    while (s->state == UNIX_STATE_CONNECTING) {
        s->waiter = g_current;
        sched_sleep();
    }

    if (s->state != UNIX_STATE_CONNECTED) {
        // Listener closed or rejected us.
        s->state = UNIX_STATE_UNCONNECTED;
        return -ECONNREFUSED;
    }

    return 0;
}

// ── unix_sock_send ───────────────────────────────────────────────────────

int unix_sock_send(vfs_file_t* f, const void* buf, uint32_t len) {
    if (!f || !buf || !len) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->shutdown_wr) return -EPIPE;

    if (s->type == SOCK_STREAM) {
        if (!s->peer) return -ENOTCONN;
        if (s->peer->shutdown_rd) return -EPIPE;

        // Write into the PEER's receive buffer.
        unix_sock_t* peer = s->peer;
        uint32_t total = 0;

        while (total < len) {
            // If peer closed, EPIPE.
            if (!s->peer || s->state == UNIX_STATE_DISCONNECTED)
                return total > 0 ? (int)total : -EPIPE;

            uint32_t wrote = cbuf_write(peer, (const uint8_t*)buf + total,
                                         len - total);
            total += wrote;

            // Wake peer if it was blocked in recv.
            if (wrote > 0) unix_wake(peer);

            // If we couldn't write everything, block until space.
            if (total < len) {
                if (peer->buf_count >= UNIX_BUF_SIZE) {
                    s->waiter = g_current;
                    sched_sleep();
                }
            }
        }
        return (int)total;
    }

    // SOCK_DGRAM: send to connected peer.
    if (!s->peer) return -ENOTCONN;
    return unix_sock_sendto(f, buf, len, NULL);
}

// ── unix_sock_recv ───────────────────────────────────────────────────────

int unix_sock_recv(vfs_file_t* f, void* buf, uint32_t len) {
    if (!f || !buf || !len) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->shutdown_rd) return 0; // EOF

    if (s->type == SOCK_STREAM) {
        // Block until data available or peer disconnects.
        while (s->buf_count == 0) {
            if (s->state == UNIX_STATE_DISCONNECTED) return 0; // EOF
            s->waiter = g_current;
            sched_sleep();
        }

        uint32_t got = cbuf_read(s, buf, len);

        // Wake peer if it was blocked on a full buffer.
        if (s->peer) unix_wake(s->peer);

        return (int)got;
    }

    // SOCK_DGRAM: dequeue one message.
    while (!s->dgram_head) {
        if (s->state == UNIX_STATE_DISCONNECTED) return 0;
        s->waiter = g_current;
        sched_sleep();
    }

    unix_dgram_t* msg = s->dgram_head;
    s->dgram_head = msg->next;
    if (!s->dgram_head) s->dgram_tail = NULL;
    s->dgram_count--;

    uint32_t copy = msg->len < len ? msg->len : len;
    uint8_t* src = UNIX_DGRAM_DATA(msg);
    uint8_t* dst = (uint8_t*)buf;
    for (uint32_t i = 0; i < copy; i++) dst[i] = src[i];

    kfree(msg);
    return (int)copy;
}

// ── unix_sock_sendto (SOCK_DGRAM) ───────────────────────────────────────

int unix_sock_sendto(vfs_file_t* f, const void* buf, uint32_t len,
                      const char* path) {
    if (!f || !buf || !len) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -EOPNOTSUPP;

    // Find the target socket.
    unix_sock_t* target;
    if (path && path[0]) {
        target = ns_find(path);
    } else {
        target = s->peer; // default peer from connect()
    }
    if (!target) return -ECONNREFUSED;
    if (target->type != SOCK_DGRAM) return -ECONNREFUSED;

    // Allocate message (header + data in one allocation).
    unix_dgram_t* msg = kmalloc(sizeof(unix_dgram_t) + len);
    if (!msg) return -ENOMEM;
    msg->next = NULL;
    msg->len  = len;

    const uint8_t* src = (const uint8_t*)buf;
    uint8_t* dst = UNIX_DGRAM_DATA(msg);
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    // Enqueue into target's receive queue.
    if (target->dgram_tail) {
        target->dgram_tail->next = msg;
    } else {
        target->dgram_head = msg;
    }
    target->dgram_tail = msg;
    target->dgram_count++;

    // Wake receiver.
    unix_wake(target);

    return (int)len;
}

// ── unix_sock_shutdown ───────────────────────────────────────────────────

int unix_sock_shutdown(vfs_file_t* f, int how) {
    if (!f) return -EBADF;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;

    // how: 0=SHUT_RD, 1=SHUT_WR, 2=SHUT_RDWR
    if (how == 0 || how == 2) {
        s->shutdown_rd = 1;
    }
    if (how == 1 || how == 2) {
        s->shutdown_wr = 1;
        // Notify peer that we won't write anymore → they see EOF on recv.
        if (s->peer) unix_wake(s->peer);
    }

    return 0;
}

// ── SCM_RIGHTS: sendfd / recvfd ──────────────────────────────────────────

int unix_sock_sendfd(vfs_file_t* sock, vfs_file_t* file, uint32_t rights) {
    if (!sock || !file) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)sock->ctx;
    if (!s) return -EBADF;
    if (!s->peer) return -ENOTCONN;

    unix_ancillary_t* anc = &s->peer->ancillary;
    if (anc->count >= UNIX_ANCILLARY_MAX) return -ENOBUFS;

    // Dup the file description and stamp attenuated rights.
    vfs_file_t* dup = vfs_dup(file);
    if (!dup) return -ENOMEM;
    dup->rights = rights;

    uint8_t idx = anc->tail;
    anc->files[idx]  = dup;
    anc->rights[idx] = rights;
    anc->tail = (anc->tail + 1) % UNIX_ANCILLARY_MAX;
    anc->count++;

    // Wake peer in case it's blocked in recvfd.
    unix_wake(s->peer);

    return 0;
}

vfs_file_t* unix_sock_recvfd(vfs_file_t* sock) {
    if (!sock) return NULL;
    unix_sock_t* s = (unix_sock_t*)sock->ctx;
    if (!s) return NULL;

    unix_ancillary_t* anc = &s->ancillary;

    // Block until an fd is available or peer disconnects.
    while (anc->count == 0) {
        if (s->state == UNIX_STATE_DISCONNECTED && !s->peer) return NULL;
        s->waiter = g_current;
        sched_sleep();
    }

    uint8_t idx = anc->head;
    vfs_file_t* file = anc->files[idx];
    anc->files[idx] = NULL;
    anc->head = (anc->head + 1) % UNIX_ANCILLARY_MAX;
    anc->count--;

    return file;
}
