#include "unix_sock.h"
#include "kheap.h"
#include "errno.h"
#include "sched.h"
#include "process.h"
#include "smp.h"
#include "rcu.h"

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

// ── Bind namespace (RCU-protected) ───────────────────────────────────────
// The whole namespace is a single object (unix_ns_table_t) published via
// rcu_assign_pointer.  Readers walk lock-free inside rcu_read_lock().
// Writers (bind/close) serialise on s_unix_ns_wlock, build a fresh table
// via copy-on-write, publish it, and call_rcu the old one.
//
// Mutation cost is O(cap) per insert/remove, but these are control-plane
// events (bind/close once per socket lifetime), while ns_find runs on
// every connect() / sendto() — so the trade is exactly what RCU is for.

#define UNIX_NS_INIT_CAP 32u

typedef struct {
    char         path[UNIX_PATH_MAX];
    unix_sock_t* sock;  // NULL = empty slot
} unix_ns_entry_t;

typedef struct unix_ns_table {
    uint32_t         cap;     // power of two
    uint32_t         cnt;
    rcu_head_t       rcu_head; // Phase 5B: embedded for call_rcu_head
    unix_ns_entry_t  slots[]; // flexible
} unix_ns_table_t;

static unix_ns_table_t* s_unix_ns       = NULL;  // RCU-protected
static spinlock_t       s_unix_ns_wlock = SPINLOCK_INIT;

static int ns_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void ns_path_copy(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < UNIX_PATH_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint32_t ns_hash_str(const char* s, uint32_t cap) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h & (cap - 1u);
}

static unix_ns_table_t* ns_table_alloc(uint32_t cap) {
    unix_ns_table_t* t = (unix_ns_table_t*)kmalloc(
        sizeof(unix_ns_table_t) + (uint64_t)cap * sizeof(unix_ns_entry_t));
    if (!t) return NULL;
    t->cap = cap;
    t->cnt = 0;
    __builtin_memset(t->slots, 0, (uint64_t)cap * sizeof(unix_ns_entry_t));
    return t;
}

static void ns_table_raw_insert(unix_ns_table_t* t, const char* path,
                                  unix_sock_t* sock) {
    uint32_t i = ns_hash_str(path, t->cap);
    while (t->slots[i].sock) i = (i + 1u) & (t->cap - 1u);
    ns_path_copy(t->slots[i].path, path);
    t->slots[i].sock = sock;
    t->cnt++;
}

static void ns_table_free_rcu(void* p) { kfree(p); }

// Reader — must be inside rcu_read_lock().  The returned pointer stays
// valid for the rest of the reader section because unix_sock_close defers
// the entire teardown via call_rcu.
static unix_sock_t* ns_find(const char* path) {
    unix_ns_table_t* t = rcu_dereference(s_unix_ns);
    if (!t) return NULL;
    uint32_t cap = t->cap;
    uint32_t i = ns_hash_str(path, cap);
    for (uint32_t n = 0; n < cap; n++) {
        unix_sock_t* s = t->slots[i].sock;
        if (!s) return NULL;
        if (ns_streq(t->slots[i].path, path)) return s;
        i = (i + 1u) & (cap - 1u);
    }
    return NULL;
}

// Writer: copy-on-write, rcu_assign_pointer, defer the old table.
static int ns_insert(const char* path, unix_sock_t* sock) {
    uint64_t flags = spin_lock_irqsave(&s_unix_ns_wlock);
    unix_ns_table_t* old = s_unix_ns;
    uint32_t old_cap = old ? old->cap : 0;
    uint32_t old_cnt = old ? old->cnt : 0;

    // Duplicate check under the writer lock.
    if (old) {
        uint32_t i = ns_hash_str(path, old_cap);
        for (uint32_t n = 0; n < old_cap; n++) {
            if (!old->slots[i].sock) break;
            if (ns_streq(old->slots[i].path, path)) {
                spin_unlock_irqrestore(&s_unix_ns_wlock, flags);
                return -EADDRINUSE;
            }
            i = (i + 1u) & (old_cap - 1u);
        }
    }

    uint32_t new_cap = old_cap ? old_cap : UNIX_NS_INIT_CAP;
    if ((old_cnt + 1u) * 4u >= new_cap * 3u)
        new_cap = old_cap ? old_cap * 2u : UNIX_NS_INIT_CAP;

    unix_ns_table_t* neu = ns_table_alloc(new_cap);
    if (!neu) { spin_unlock_irqrestore(&s_unix_ns_wlock, flags); return -ENOMEM; }

    if (old) {
        for (uint32_t i = 0; i < old->cap; i++)
            if (old->slots[i].sock)
                ns_table_raw_insert(neu, old->slots[i].path, old->slots[i].sock);
    }
    ns_table_raw_insert(neu, path, sock);

    rcu_assign_pointer(s_unix_ns, neu);
    spin_unlock_irqrestore(&s_unix_ns_wlock, flags);
    if (old) call_rcu_head(&old->rcu_head, ns_table_free_rcu, old);
    return 0;
}

static void ns_remove(const char* path) {
    uint64_t flags = spin_lock_irqsave(&s_unix_ns_wlock);
    unix_ns_table_t* old = s_unix_ns;
    if (!old) { spin_unlock_irqrestore(&s_unix_ns_wlock, flags); return; }

    unix_ns_table_t* neu = ns_table_alloc(old->cap);
    if (!neu) { spin_unlock_irqrestore(&s_unix_ns_wlock, flags); return; }
    int removed = 0;
    for (uint32_t i = 0; i < old->cap; i++) {
        if (!old->slots[i].sock) continue;
        if (!removed && ns_streq(old->slots[i].path, path)) {
            removed = 1;
            continue;
        }
        ns_table_raw_insert(neu, old->slots[i].path, old->slots[i].sock);
    }

    rcu_assign_pointer(s_unix_ns, neu);
    spin_unlock_irqrestore(&s_unix_ns_wlock, flags);
    call_rcu_head(&old->rcu_head, ns_table_free_rcu, old);
}

// ── Internal helpers ─────────────────────────────────────────────────────

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void zero_mem(void* p, uint32_t n) {
    __builtin_memset(p, 0, n);
}

// Wake every task sleeping on a socket (blocking recv/accept/connect/send).
static void unix_wake(unix_sock_t* s) {
    wait_queue_wake_all(&s->waitq);
}

// Wake all tasks sleeping in poll/epoll on this socket's vfs_file.
static void unix_poll_wake(unix_sock_t* s) {
    if (s->file) wait_queue_wake_all(s->file->waitq);
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
        if (s->type == SOCK_STREAM) {
            // Read the peer once under rcu_read_lock: a concurrent close
            // defers the peer's free past the grace period, so the pointer
            // stays valid for this section (and close clears it before free,
            // so it is never a dangling pointer to freed memory).
            rcu_read_lock();
            unix_sock_t* peer = s->peer;
            int writable = peer && peer->buf_count < UNIX_BUF_SIZE;
            rcu_read_unlock();
            if (writable) return 1;
        }
        if (s->type == SOCK_DGRAM) return 1; // dgram always writable
    }
    if (events & 0x0010 /* POLLHUP */) {
        // Peer closed its end.  poll/epoll users (e.g. sway's IPC server)
        // rely on POLLHUP to stop polling a dead fd; without it they see the
        // EOF socket as perpetually POLLIN-readable, read 0 bytes, and
        // busy-loop forever.  Report HUP once the peer is gone.
        if (s->state == UNIX_STATE_DISCONNECTED) return 1;
    }
    return 0;
}

// ── Reference counting ────────────────────────────────────────────────────
// The socket is freed via call_rcu only when its refcount reaches 0.  A peer
// that touches this socket across a blocking operation (unix_sock_send_ex) or
// in a brief synchronous critical section (recv drain wake, poll, shutdown,
// sendfd) pins it with an extra ref so a concurrent close cannot free it out
// from under the operation -> closes the peer-SOCK use-after-free (T3 part B).

// Pure, side-effect-only-on-*rc helper: try to bump a refcount from a NON-ZERO
// value.  Returns 1 on success (incremented), 0 if it was already 0 (object is
// dying / freed-pending and must not be resurrected).  Unit-tested below.
static int unix_refcount_tryget(uint32_t* rc) {
    uint32_t old = atomic_load_acq(rc);
    for (;;) {
        if (old == 0) return 0;                       // dying: cannot resurrect
        if (atomic_cas(rc, &old, old + 1u)) return 1; // bumped
        // CAS failed: `old` reloaded with the current value; retry.
    }
}

static void unix_sock_free_rcu(void* data);  // forward decl

static inline void unix_get(unix_sock_t* s) {
    atomic_add(&s->refcount, 1u);
}

// Drop a reference.  The CPU that observes the 1->0 transition owns the
// teardown and defers it a full grace period via call_rcu (so any reader still
// inside an rcu_read_lock that observed this socket has dropped out first).
static void unix_put(unix_sock_t* s) {
    if (atomic_sub(&s->refcount, 1u) == 1u)
        call_rcu_expedited(unix_sock_free_rcu, s);
}

// Read s->peer and pin it (hold a ref) so it stays valid across a blocking or
// synchronous critical section.  Returns the pinned peer or NULL (no peer, or
// peer already dying).  Memory-safe: the peer is RCU-freed, so reading its
// refcount inside this rcu_read_lock section cannot race the free; and
// unix_sock_close clears the symmetric back-pointer BEFORE the peer can be
// freed, so s->peer never dangles to already-freed memory.  Caller unix_put()s.
static unix_sock_t* unix_pin_peer(unix_sock_t* s) {
    rcu_read_lock();
    unix_sock_t* peer = s->peer;
    if (peer && !unix_refcount_tryget(&peer->refcount))
        peer = NULL;
    rcu_read_unlock();
    return peer;
}

// Deferred teardown: runs after a full RCU grace period has elapsed, so
// every concurrent ns_find/connect/sendto that observed the sock has
// dropped out of its reader section.  Safe to dismantle state and free.
// The connected-peer disconnect notify is NOT done here -- it runs
// synchronously in unix_sock_close so the peer's back-pointer is cleared
// before this socket can be freed (the invariant unix_pin_peer relies on).
static void unix_sock_free_rcu(void* data) {
    unix_sock_t* s = (unix_sock_t*)data;

    // Free pending connections in backlog.
    unix_pending_t* p = s->backlog_head;
    while (p) {
        unix_pending_t* next = p->next;
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

    // The vfs_file_t wrapper shares the socket's lifetime: a peer reaches it
    // via peer->file for poll wakeups (unix_poll_wake derefs ->file->waitq),
    // so it must outlive every RCU reader that can still observe this socket.
    // Freeing it here (not eagerly in unix_sock_close) closes the ->file
    // use-after-free: the file now lives exactly as long as its unix_sock_t.
    struct vfs_file_t* file = s->file;
    kfree(s);
    if (file) kfree(file);
}

void unix_sock_close(vfs_file_t* self) {
    extern void kprintf(const char*, ...);
    unix_sock_t* s = (unix_sock_t*)self->ctx;
    if (!s) {
        kprintf("[unix] close: no ctx on file %p (nothing to free)\n",
                (void*)self);
        kfree(self); return;
    }

    /* Unconditional entry log so we can see every unix sock close,
     * whether it's bound or not.  (Observed: second dwl's bind got
     * EADDRINUSE on /tmp/wayland-0 even though the first dwl was
     * dead — need to know if the close ran at all.) */
    kprintf("[unix] close: path=\"%s\" state=%u type=%d %s\n",
            s->path[0] ? s->path : "(unbound)",
            (unsigned)s->state, (int)s->type,
            s->path[0] ? "evicting from ns" : "not in ns");

    // Unpublish from the namespace first so new ns_find() cannot observe
    // the sock after this point.  Readers that observed the sock in a
    // prior rcu_read_lock() are still safe until they drop out — we defer
    // the actual teardown via call_rcu (in unix_put) below.
    if (s->path[0])
        ns_remove(s->path);

    // Notify the connected peer SYNCHRONOUSLY, before we can be freed.  A peer
    // that pins us via unix_pin_peer relies on the back-pointer being cleared
    // before the free, so it observes NULL (not dangling) once we are gone.
    // Pin the peer so this mutation can't race its own free.  Only clear the
    // back-pointer for a SYMMETRIC stream connection (peer->peer == s); a
    // SOCK_DGRAM default-destination peer does not point back at us and must
    // keep its own linkage.
    unix_sock_t* peer = unix_pin_peer(s);
    if (peer) {
        if (peer->peer == s) {
            peer->peer  = NULL;
            peer->state = UNIX_STATE_DISCONNECTED;
        }
        unix_wake(peer);
        unix_poll_wake(peer);
        unix_put(peer);
    }

    // Drop the owning reference.  The socket (and its vfs_file_t `self`, per
    // the F14 lifetime invariant) is freed via call_rcu once the last ref --
    // including any transient peer pins -- is gone.  `self` is NOT freed here:
    // peers reach it via peer->file for poll wakeups while the socket is still
    // observable, so its free is deferred to unix_sock_free_rcu.
    unix_put(s);
}

// ── unix_sock_ioctl ──────────────────────────────────────────────────────
// FIONREAD = _IOR('T', 0x1b, int): number of bytes available to read now.
// sway's IPC server calls ioctl(fd, FIONREAD, &n) to size each request read;
// with a NULL ioctl the call failed and sway never read the queued command,
// so swaybar (and any i3-IPC client) hung forever in ipc_initialize.
#define UNIX_FIONREAD 0x541b

int64_t unix_sock_ioctl(vfs_file_t* f, uint64_t request, uint64_t arg) {
    extern int copy_to_user(void* dst_u, const void* src, uint64_t len);
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if ((uint32_t)request == UNIX_FIONREAD) {
        // Mirror what a subsequent recv() would return: SOCK_STREAM reads
        // from this socket's own circular buffer; SOCK_DGRAM returns the
        // size of the next queued datagram (POSIX semantics).
        int navail;
        if (s->type == SOCK_DGRAM)
            navail = s->dgram_head ? (int)s->dgram_head->len : 0;
        else
            navail = (int)s->buf_count;
        if (copy_to_user((void*)arg, &navail, sizeof(navail)) != 0)
            return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

// ── unix_sock_open ───────────────────────────────────────────────────────

vfs_file_t* unix_sock_open(int type) {
    // Strip SOCK_CLOEXEC / SOCK_NONBLOCK (Linux-style OR flags) before
    // matching the core type.  wl_display_add_socket_auto passes
    // SOCK_STREAM | SOCK_CLOEXEC; without this mask, it failed with
    // -ENOMEM (propagated from our NULL return below).
    int sock_type = type & ~(0x80000 | 0x00800);  // SOCK_CLOEXEC | SOCK_NONBLOCK
    if (sock_type != SOCK_STREAM && sock_type != SOCK_DGRAM) return NULL;

    unix_sock_t* s = kmalloc(sizeof(unix_sock_t));
    if (!s) return NULL;
    zero_mem(s, sizeof(unix_sock_t));
    wait_queue_init(&s->waitq);

    s->type  = (uint8_t)sock_type;
    s->state = UNIX_STATE_UNCONNECTED;
    s->refcount = 1;   // the owning vfs_file_t holds the first reference

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(s); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));

    f->read        = unix_vfs_read;
    f->write       = unix_vfs_write;
    f->close       = unix_sock_close;
    f->seek        = NULL;
    f->poll           = unix_vfs_poll;
    f->ioctl          = unix_sock_ioctl;
    f->ctx            = s;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount    = 1;
    f->rights      = 0xFFFFFFFF;
    f->path[0]     = '\0';

    s->file = f;  // back-pointer for poll wakeups

    return f;
}

// ── unix_sock_pair ───────────────────────────────────────────────────────
// Creates two connected sockets — the POSIX socketpair() primitive.
// Linux-compatible: both ends are unbound (no filesystem path), fully
// peer-linked, and immediately in UNIX_STATE_CONNECTED.  No listen/accept
// dance, no backlog.  Used heavily by wayland/pthread for fd passing.
int unix_sock_pair(int type, vfs_file_t** out) {
    if (!out) return -EINVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;

    vfs_file_t* a = unix_sock_open(type);
    if (!a) return -ENOMEM;
    vfs_file_t* b = unix_sock_open(type);
    // unix_sock_close owns the full teardown of `a` (including deferring the
    // vfs_file_t free); do NOT also kfree(a) here -- that was a double free.
    if (!b) { unix_sock_close(a); return -ENOMEM; }

    unix_sock_t* sa = (unix_sock_t*)a->ctx;
    unix_sock_t* sb = (unix_sock_t*)b->ctx;

    sa->peer  = sb;
    sb->peer  = sa;
    sa->state = UNIX_STATE_CONNECTED;
    sb->state = UNIX_STATE_CONNECTED;
    // Both ends are in the same process — peer_pid is self on each side.
    sa->peer_pid = g_current ? g_current->pid : 0;
    sb->peer_pid = sa->peer_pid;

    out[0] = a;
    out[1] = b;
    return 0;
}

// ── socketpair selftest ──────────────────────────────────────────────
// Uses unix_sock_pair + the peer linkage directly (bypassing fd_install
// since init_kthread doesn't have a regular user fd table).  Exercises
// bidirectional send/recv over the pair — the most common wayland path.
extern void kprintf_atomic(const char* fmt, ...);
void socketpair_selftest(void) {
    vfs_file_t* pair[2];
    if (unix_sock_pair(SOCK_STREAM, pair) != 0) {
        kprintf_atomic("[socketpair-selftest] FAIL pair alloc\n");
        return;
    }
    const char msg_ab[] = "ping";
    const char msg_ba[] = "pong";
    char rx[8] = {0};

    // A → B
    int r = unix_sock_send(pair[0], msg_ab, 4);
    if (r != 4) {
        kprintf_atomic("[socketpair-selftest] FAIL send A->B = %d\n", r);
        goto fail;
    }
    r = unix_sock_recv(pair[1], rx, 4);
    if (r != 4 || rx[0]!='p' || rx[1]!='i' || rx[2]!='n' || rx[3]!='g') {
        kprintf_atomic("[socketpair-selftest] FAIL recv A->B r=%d\n", r);
        goto fail;
    }

    // B → A
    __builtin_memset(rx, 0, sizeof(rx));
    r = unix_sock_send(pair[1], msg_ba, 4);
    if (r != 4) {
        kprintf_atomic("[socketpair-selftest] FAIL send B->A = %d\n", r);
        goto fail;
    }
    r = unix_sock_recv(pair[0], rx, 4);
    if (r != 4 || rx[0]!='p' || rx[1]!='o' || rx[2]!='n' || rx[3]!='g') {
        kprintf_atomic("[socketpair-selftest] FAIL recv B->A r=%d\n", r);
        goto fail;
    }

    unix_sock_close(pair[0]);
    unix_sock_close(pair[1]);
    kprintf_atomic("[socketpair-selftest] PASS (bidirectional stream)\n");
    return;
fail:
    unix_sock_close(pair[0]);
    unix_sock_close(pair[1]);
}

// ── SCM_RIGHTS ancillary selftest ────────────────────────────────────
// Exercises unix_sock_sendfd → unix_sock_recvfd round-trip on a pair.
// This is the kernel half of what sendmsg(SCM_RIGHTS)/recvmsg parse
// into — userland msghdr marshalling is validated once a userland
// test app runs (wayland handshake, etc).
//
// Uses a SEPARATE standalone unix_sock as the payload — passing one
// end of the transport pair would alias the free path on cleanup.
void scm_rights_selftest(void) {
    vfs_file_t* pair[2];
    if (unix_sock_pair(SOCK_STREAM, pair) != 0) {
        kprintf_atomic("[scm_rights-selftest] FAIL pair alloc\n");
        return;
    }
    vfs_file_t* payload = unix_sock_open(SOCK_DGRAM);
    if (!payload) {
        kprintf_atomic("[scm_rights-selftest] FAIL payload alloc\n");
        unix_sock_close(pair[0]);
        unix_sock_close(pair[1]);
        return;
    }
    // Hand payload to the sender.  unix_sock_sendfd does not bump
    // refcount — it transfers ownership into the ancillary queue.
    // We must NOT close `payload` directly; the receiver's close
    // is authoritative.
    int r = unix_sock_sendfd(pair[0], payload, payload->rights);
    if (r < 0) {
        kprintf_atomic("[scm_rights-selftest] FAIL sendfd = %d\n", r);
        unix_sock_close(payload);
        goto fail;
    }
    vfs_file_t* got = unix_sock_recvfd(pair[1]);
    if (!got) {
        kprintf_atomic("[scm_rights-selftest] FAIL recvfd NULL\n");
        goto fail;
    }
    if (got != payload) {
        kprintf_atomic("[scm_rights-selftest] FAIL recvfd != payload\n");
        got->close(got);
        goto fail;
    }
    // Close the received file — this is the only reference now.
    got->close(got);

    unix_sock_close(pair[0]);
    unix_sock_close(pair[1]);
    kprintf_atomic("[scm_rights-selftest] PASS (fd round-trip over pair)\n");
    return;
fail:
    unix_sock_close(pair[0]);
    unix_sock_close(pair[1]);
}

// ── refcount / peer-pin selftest (T3 part B) ─────────────────────────────
// Deterministic checks of the lifetime machinery that closes the peer-SOCK
// use-after-free: the pure CAS-from-nonzero tryget, get/put balance on a live
// socket, and unix_pin_peer returning the connected peer with a held ref.
void unix_refcount_selftest(void) {
    int fails = 0;

    // 1. Pure tryget: bumps from non-zero, refuses to resurrect from zero.
    uint32_t rc = 1;
    if (unix_refcount_tryget(&rc) != 1 || rc != 2) {
        kprintf_atomic("[unix_refcount-selftest] FAIL tryget(1) rc=%lu\n",
                       (unsigned long)rc);
        fails++;
    }
    if (unix_refcount_tryget(&rc) != 1 || rc != 3) {
        kprintf_atomic("[unix_refcount-selftest] FAIL tryget(2) rc=%lu\n",
                       (unsigned long)rc);
        fails++;
    }
    rc = 0;
    if (unix_refcount_tryget(&rc) != 0 || rc != 0) {
        kprintf_atomic("[unix_refcount-selftest] FAIL tryget(0) resurrected rc=%lu\n",
                       (unsigned long)rc);
        fails++;
    }

    // 2. get/put balance on a live socket (refcount starts at 1 = owner).
    vfs_file_t* f = unix_sock_open(SOCK_STREAM);
    if (!f) {
        kprintf_atomic("[unix_refcount-selftest] FAIL open\n");
        kprintf_atomic("[unix_refcount-selftest] SELF-TEST FAILED\n");
        return;
    }
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (s->refcount != 1) {
        kprintf_atomic("[unix_refcount-selftest] FAIL init rc=%lu\n",
                       (unsigned long)s->refcount);
        fails++;
    }
    unix_get(s);
    if (s->refcount != 2) {
        kprintf_atomic("[unix_refcount-selftest] FAIL get rc=%lu\n",
                       (unsigned long)s->refcount);
        fails++;
    }
    unix_put(s);                 // back to 1 (owner) -- NOT freed
    if (s->refcount != 1) {
        kprintf_atomic("[unix_refcount-selftest] FAIL put rc=%lu\n",
                       (unsigned long)s->refcount);
        fails++;
    }
    unix_sock_close(f);          // drops the owner ref -> RCU free

    // 3. pin_peer on a connected pair returns the peer with a held ref.
    vfs_file_t* pair[2];
    if (unix_sock_pair(SOCK_STREAM, pair) == 0) {
        unix_sock_t* s0 = (unix_sock_t*)pair[0]->ctx;
        unix_sock_t* s1 = (unix_sock_t*)pair[1]->ctx;
        unix_sock_t* pinned = unix_pin_peer(s0);
        if (pinned != s1 || s1->refcount != 2) {
            kprintf_atomic("[unix_refcount-selftest] FAIL pin pinned=%p s1=%p rc=%lu\n",
                           (void*)pinned, (void*)s1, (unsigned long)s1->refcount);
            fails++;
        }
        if (pinned) unix_put(pinned);   // back to 1
        if (s1->refcount != 1) {
            kprintf_atomic("[unix_refcount-selftest] FAIL unpin rc=%lu\n",
                           (unsigned long)s1->refcount);
            fails++;
        }
        unix_sock_close(pair[0]);
        unix_sock_close(pair[1]);
    } else {
        kprintf_atomic("[unix_refcount-selftest] FAIL pair alloc\n");
        fails++;
    }

    kprintf_atomic(fails ? "[unix_refcount-selftest] SELF-TEST FAILED\n"
                         : "[unix_refcount-selftest] PASS (tryget + get/put balance + pin_peer)\n");
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

    // Block until a pending connection is available.  Canonical
    // Phase 9-6 pattern: task_we_add BEFORE the re-check, remove on
    // every exit path (including EINTR), commit-before-wake on the
    // connect() side (which calls unix_wake(target) after installing
    // pend in backlog_head).
    WAIT_EVENT_HOOK(&listener->waitq,
                    listener->backlog_head != NULL
                        || listener->state != UNIX_STATE_LISTENING,
                    if (signal_has_actionable(&g_current->sigstate))
                        return NULL;);
    if (listener->state != UNIX_STATE_LISTENING) return NULL;

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

    // Stamp trusted peer pids. The server side's peer is the connecting
    // process (whose pid was recorded on the client sock during connect()).
    // The client side's peer is the accepting process (g_current).
    server->peer_pid = client->peer_pid;   // connector pid (set in connect())
    client->peer_pid = g_current->pid;     // acceptor pid

    // Wake the client blocked in connect().
    unix_wake(client);

    return server_f;
}

// ── unix_sock_connect ────────────────────────────────────────────────────

int unix_sock_connect(vfs_file_t* f, const char* path) {
    if (!f || !path || !path[0]) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;

    // Reader section spans the namespace lookup AND the subsequent mutation
    // of target->backlog / target->peer.  A concurrent close() on target
    // defers the actual teardown via call_rcu, so `target` stays valid
    // until we rcu_read_unlock.  Nothing inside the section sleeps.
    rcu_read_lock();
    unix_sock_t* target = ns_find(path);
    if (!target) { rcu_read_unlock(); return -ECONNREFUSED; }

    if (s->type == SOCK_DGRAM) {
        // SOCK_DGRAM: just remember the default destination.
        s->peer = target;
        s->state = UNIX_STATE_CONNECTED;
        rcu_read_unlock();
        return 0;
    }

    // SOCK_STREAM: queue ourselves on the listener's backlog.
    if (s->state != UNIX_STATE_UNCONNECTED && s->state != UNIX_STATE_BOUND) {
        rcu_read_unlock(); return -EISCONN;
    }
    if (target->type != SOCK_STREAM) { rcu_read_unlock(); return -ECONNREFUSED; }
    if (target->state != UNIX_STATE_LISTENING) { rcu_read_unlock(); return -ECONNREFUSED; }
    if (target->backlog_count >= target->backlog_max) { rcu_read_unlock(); return -ECONNREFUSED; }

    // Stash our own pid on the client sock so accept() can propagate it to
    // the server side as the trusted peer pid (accept() overwrites our copy
    // with the acceptor's pid right after).
    s->peer_pid = g_current->pid;

    // Enqueue ourselves.
    unix_pending_t* pend = kmalloc(sizeof(unix_pending_t));
    if (!pend) { rcu_read_unlock(); return -ENOMEM; }
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

    // Wake the listener if it's blocked in accept() or poll().
    unix_wake(target);
    unix_poll_wake(target);

    // Drop the RCU reader section before sleeping.  From here on we only
    // touch our own sock `s`; the listener owns `pend` and, if it closes,
    // its deferred teardown walks the backlog and wakes us with state set
    // to UNCONNECTED.
    rcu_read_unlock();

    // Block until accept() completes the pairing (or listener closes).
    // Canonical Phase 9-6 pattern; always remove on exit, including
    // EINTR path below.
    WAIT_EVENT_HOOK(&s->waitq,
                    s->state != UNIX_STATE_CONNECTING,
                    if (signal_has_actionable(&g_current->sigstate))
                        return -EINTR;);

    if (s->state != UNIX_STATE_CONNECTED) {
        // Listener closed or rejected us.
        s->state = UNIX_STATE_UNCONNECTED;
        return -ECONNREFUSED;
    }

    return 0;
}

// ── unix_sock_send ───────────────────────────────────────────────────────

int unix_sock_send_ex(vfs_file_t* f, const void* buf, uint32_t len,
                      int nonblock) {
    if (!f) return -EINVAL;
    if (len == 0) return 0;   // POSIX: a 0-length read/write is a successful no-op
    if (!buf) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->shutdown_wr) return -EPIPE;

    if (s->type == SOCK_STREAM) {
        // Pin the peer for the WHOLE call.  We cache and dereference `peer`
        // (cbuf_write, the WAIT_EVENT_HOOK buf_count condition, the wakes)
        // across a blocking sleep, so it must not be freed by a concurrent
        // close mid-send.  The pin holds it alive; we still consult our own
        // s->peer / s->state to detect a disconnect (close clears those).
        unix_sock_t* peer = unix_pin_peer(s);
        if (!peer) return -ENOTCONN;
        if (peer->shutdown_rd) { unix_put(peer); return -EPIPE; }

        uint32_t total = 0;
        int ret;

        while (total < len) {
            // If peer closed, EPIPE.  (close() clears s->peer + sets state.)
            if (!s->peer || s->state == UNIX_STATE_DISCONNECTED) {
                ret = total > 0 ? (int)total : -EPIPE;
                goto stream_out;
            }

            uint32_t wrote = cbuf_write(peer, (const uint8_t*)buf + total,
                                         len - total);
            total += wrote;

            // Wake peer if it was blocked in recv or poll.
            if (wrote > 0) {
                unix_wake(peer);
                unix_poll_wake(peer);
            }

            // If we couldn't write everything, either return EAGAIN
            // (nonblocking) or block until space.  Canonical Phase 9-6
            // pattern: task_we_add → re-check → sched_sleep → remove.
            // commit-before-wake on the peer's recv drain side
            // (unix_sock_recv fires unix_wake(s->peer) after cbuf_read).
            if (total < len && peer->buf_count >= UNIX_BUF_SIZE) {
                if ((f->flags & O_NONBLOCK) || nonblock) {
                    ret = total > 0 ? (int)total : -EAGAIN;
                    goto stream_out;
                }
                WAIT_EVENT_HOOK(&s->waitq,
                                peer->buf_count < UNIX_BUF_SIZE
                                    || !s->peer
                                    || s->state == UNIX_STATE_DISCONNECTED,
                                if (signal_has_actionable(&g_current->sigstate)) {
                                    ret = total > 0 ? (int)total : -EINTR;
                                    goto stream_out;
                                });
            }
        }
        ret = (int)total;
stream_out:
        unix_put(peer);
        return ret;
    }

    // SOCK_DGRAM: send to connected peer.
    if (!s->peer) return -ENOTCONN;
    return unix_sock_sendto(f, buf, len, NULL);
}

int unix_sock_send(vfs_file_t* f, const void* buf, uint32_t len) {
    return unix_sock_send_ex(f, buf, len, 0);
}

// ── unix_sock_recv ───────────────────────────────────────────────────────

int unix_sock_recv_ex(vfs_file_t* f, void* buf, uint32_t len, int nonblock) {
    if (!f) return -EINVAL;
    if (len == 0) return 0;   // POSIX: a 0-length read/write is a successful no-op
    if (!buf) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->shutdown_rd) return 0; // EOF

    if (s->type == SOCK_STREAM) {
        // Block until data available or peer disconnects.  Canonical
        // Phase 9-6 pattern; commit-before-wake on the peer's send
        // side (unix_sock_send calls unix_wake(peer) after cbuf_write).
        if (s->buf_count == 0) {
            if (s->state == UNIX_STATE_DISCONNECTED) return 0; // EOF
            if ((f->flags & O_NONBLOCK) || nonblock) return -EAGAIN;
            WAIT_EVENT_HOOK(&s->waitq,
                            s->buf_count != 0
                                || s->state == UNIX_STATE_DISCONNECTED,
                            if (signal_has_actionable(&g_current->sigstate))
                                return -EINTR;);
            if (s->buf_count == 0) return 0; // EOF
        }

        uint32_t got = cbuf_read(s, buf, len);

        // Drain committed: now wake the peer so a blocked sender
        // observes the newly-freed space.  ACQ_REL on wake_all pairs
        // with the sender's subsequent rq_lock acquire in sched_sleep.
        // rcu_read_lock keeps the peer alive for the wake against a
        // concurrent close (RCU-deferred free + clear-before-free).
        rcu_read_lock();
        if (s->peer) unix_wake(s->peer);
        rcu_read_unlock();

        return (int)got;
    }

    // SOCK_DGRAM: dequeue one message.
    if (!s->dgram_head) {
        if (s->state == UNIX_STATE_DISCONNECTED) return 0;
        if ((f->flags & O_NONBLOCK) || nonblock) return -EAGAIN;
        WAIT_EVENT_HOOK(&s->waitq,
                        s->dgram_head != NULL
                            || s->state == UNIX_STATE_DISCONNECTED,
                        if (signal_has_actionable(&g_current->sigstate))
                            return -EINTR;);
        if (!s->dgram_head) return 0;
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

int unix_sock_recv(vfs_file_t* f, void* buf, uint32_t len) {
    return unix_sock_recv_ex(f, buf, len, 0);
}

// ── unix_sock_sendto (SOCK_DGRAM) ───────────────────────────────────────

int unix_sock_sendto(vfs_file_t* f, const void* buf, uint32_t len,
                      const char* path) {
    if (!f) return -EINVAL;
    if (len == 0) return 0;   // POSIX: a 0-length read/write is a successful no-op
    if (!buf) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -EOPNOTSUPP;

    // Reader section: ns_find and all subsequent derefs of target.  No
    // sleeps inside (kmalloc is non-blocking).  If target is closed
    // concurrently, its teardown is deferred via call_rcu so the pointer
    // remains valid for the rest of this section.
    rcu_read_lock();
    unix_sock_t* target;
    if (path && path[0]) {
        target = ns_find(path);
    } else {
        target = s->peer; // default peer from connect()
    }
    if (!target) { rcu_read_unlock(); return -ECONNREFUSED; }
    if (target->type != SOCK_DGRAM) { rcu_read_unlock(); return -ECONNREFUSED; }

    // Allocate message (header + data in one allocation).
    unix_dgram_t* msg = kmalloc(sizeof(unix_dgram_t) + len);
    if (!msg) { rcu_read_unlock(); return -ENOMEM; }
    msg->next = NULL;
    msg->len  = len;

    __builtin_memcpy(UNIX_DGRAM_DATA(msg), buf, len);

    // Enqueue into target's receive queue.
    if (target->dgram_tail) {
        target->dgram_tail->next = msg;
    } else {
        target->dgram_head = msg;
    }
    target->dgram_tail = msg;
    target->dgram_count++;

    // Wake receiver (blocking recv or poll).
    unix_wake(target);
    unix_poll_wake(target);

    rcu_read_unlock();
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
        // rcu_read_lock pins the peer against a concurrent close.
        rcu_read_lock();
        unix_sock_t* peer = s->peer;
        if (peer) {
            unix_wake(peer);
            unix_poll_wake(peer);
        }
        rcu_read_unlock();
    }

    return 0;
}

// ── SCM_RIGHTS: sendfd / recvfd ──────────────────────────────────────────

int unix_sock_sendfd(vfs_file_t* sock, vfs_file_t* file, uint32_t rights) {
    if (!sock || !file) return -EINVAL;
    unix_sock_t* s = (unix_sock_t*)sock->ctx;
    if (!s) return -EBADF;

    // Pin the peer for the whole body: we mutate its ancillary queue across a
    // vfs_dup, so it must not be freed by a concurrent close mid-enqueue.
    unix_sock_t* peer = unix_pin_peer(s);
    if (!peer) return -ENOTCONN;

    unix_ancillary_t* anc = &peer->ancillary;
    if (anc->count >= UNIX_ANCILLARY_MAX) { unix_put(peer); return -ENOBUFS; }

    // Dup the file description and stamp attenuated rights.
    vfs_file_t* dup = vfs_dup(file);
    if (!dup) { unix_put(peer); return -ENOMEM; }
    dup->rights = rights;

    uint8_t idx = anc->tail;
    anc->files[idx]  = dup;
    anc->rights[idx] = rights;
    anc->tail = (anc->tail + 1) % UNIX_ANCILLARY_MAX;
    anc->count++;

    // Wake peer in case it's blocked in recvfd or poll.
    unix_wake(peer);
    unix_poll_wake(peer);

    unix_put(peer);
    return 0;
}

// Non-blocking ancillary dequeue.  recvmsg's SCM_RIGHTS drain runs on
// EVERY recvmsg that supplies a control buffer (libwayland always
// does); blocking here parked dwl inside its first recvmsg waiting on
// `anc->count != 0` while foot's payload sat unread in the stream
// buffer — the wake fired, the condition stayed false, the compositor
// slept forever.
vfs_file_t* unix_sock_recvfd_nb(vfs_file_t* sock) {
    if (!sock) return NULL;
    unix_sock_t* s = (unix_sock_t*)sock->ctx;
    if (!s) return NULL;
    unix_ancillary_t* anc = &s->ancillary;
    if (anc->count == 0) return NULL;

    uint8_t idx = anc->head;
    vfs_file_t* file = anc->files[idx];
    anc->files[idx] = NULL;
    anc->head = (anc->head + 1) % UNIX_ANCILLARY_MAX;
    anc->count--;
    return file;
}

vfs_file_t* unix_sock_recvfd(vfs_file_t* sock) {
    if (!sock) return NULL;
    unix_sock_t* s = (unix_sock_t*)sock->ctx;
    if (!s) return NULL;

    unix_ancillary_t* anc = &s->ancillary;

    // Block until an fd is available or peer disconnects.  Canonical
    // Phase 9-6 pattern; caller maps NULL on signal interruption to
    // whatever errno it prefers (sys_recvfd → -EINTR path).
    if (anc->count == 0) {
        if (s->state == UNIX_STATE_DISCONNECTED && !s->peer) return NULL;
        WAIT_EVENT_HOOK(&s->waitq,
                        anc->count != 0
                            || (s->state == UNIX_STATE_DISCONNECTED && !s->peer),
                        if (signal_has_actionable(&g_current->sigstate))
                            return NULL;);
        if (anc->count == 0) return NULL;
    }

    uint8_t idx = anc->head;
    vfs_file_t* file = anc->files[idx];
    anc->files[idx] = NULL;
    anc->head = (anc->head + 1) % UNIX_ANCILLARY_MAX;
    anc->count--;

    return file;
}
