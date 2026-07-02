#include "unix_sock.h"
#include "kheap.h"
#include "errno.h"
#include "ilist.h"      // FIFO_ENQUEUE_TAIL / FIFO_DEQUEUE_HEAD (dgram + accept-backlog queues)
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

// Serializes the STREAM pairing state machine: accept()'s claim+peer-link, the
// connect()/close() bail of a still-CONNECTING client, and close()'s symmetric
// peer back-pointer clear.  These all mutate a backlog client's `state` and the
// `peer` pointers; without one lock, accept linking server->peer = client races
// a concurrent close() on the client (the fd is shared, connect uses fd_to_file)
// that reads client->peer == NULL before the link and so never clears
// server->peer -> server->peer dangles once the client is freed.  Holding this
// across [claim + link] and [state-read + peer-clear] makes "state == CONNECTED"
// imply "peers fully linked", so close always observes a consistent pair.  Leaf
// lock: every alloc and wake happens OUTSIDE it, so no nesting / deadlock.
static spinlock_t       s_unix_pair_lock = SPINLOCK_INIT;

// Bounded equality.  Every path here lives in a UNIX_PATH_MAX buffer and is
// NUL-terminated by construction (ns_path_copy caps stored paths; the syscall
// boundary copy_sockaddr_un_from_user NUL-caps incoming ones).  Capping the walk
// at UNIX_PATH_MAX makes it safe-by-construction -- it can never run off the end
// even if a future caller forgets to terminate.  For NUL-terminated inputs the
// result is identical to the old byte-for-byte compare.
static int ns_streq(const char* a, const char* b) {
    for (uint32_t i = 0; i < UNIX_PATH_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static void ns_path_copy(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < UNIX_PATH_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Bounded FNV-1a.  Capped at UNIX_PATH_MAX (paths never exceed it); for any
// NUL-terminated path shorter than the cap the hash is identical to the old
// unbounded walk, so the namespace hashing is unchanged for every real path.
static uint32_t ns_hash_str(const char* s, uint32_t cap) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < UNIX_PATH_MAX && s[i]; i++) {
        h ^= (uint8_t)s[i]; h *= 16777619u;
    }
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

// Move a chunk into/out of the OWNER's ring UNDER the owner's per-socket lock.
// `kdata` MUST be a KERNEL buffer: the caller bounces user data in/out OUTSIDE
// the lock (a user-memory access must never fault under a preempt-disabled
// spinlock) in bounded chunks that also cap the lock-hold time.  The byte copy +
// buf_count RMW + index advance are now atomic w.r.t. the peer, so a sender's
// `buf_count +=` (cbuf_write_locked on the receiver) can no longer race the
// owner's `buf_count -=` (cbuf_read_locked on itself) -- the permanent fill-level
// corruption + torn stream bytes this used to allow.  Both ends lock the same
// socket (the buffer owner); A->B and B->A use different locks (no AB-BA).
static uint32_t cbuf_write_locked(unix_sock_t* s, const uint8_t* kdata, uint32_t len) {
    spin_lock(&s->lock);
    uint32_t avail = UNIX_BUF_SIZE - s->buf_count;
    if (len > avail) len = avail;
    for (uint32_t i = 0; i < len; i++) {
        s->buf[s->buf_tail] = kdata[i];
        s->buf_tail = (s->buf_tail + 1) & (UNIX_BUF_SIZE - 1);
    }
    s->buf_count += len;
    spin_unlock(&s->lock);
    return len;
}

static uint32_t cbuf_read_locked(unix_sock_t* s, uint8_t* kdata, uint32_t len) {
    spin_lock(&s->lock);
    if (len > s->buf_count) len = s->buf_count;
    for (uint32_t i = 0; i < len; i++) {
        kdata[i] = s->buf[s->buf_head];
        s->buf_head = (s->buf_head + 1) & (UNIX_BUF_SIZE - 1);
    }
    s->buf_count -= len;
    spin_unlock(&s->lock);
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

    // Free pending connections in backlog.  Each entry owns a ref on its
    // client (unix_get in connect); wake the blocked connector with an error,
    // then drop that ref (the listener never accepted it).
    unix_pending_t* p = s->backlog_head;
    while (p) {
        unix_pending_t* next = p->next;
        if (p->client) {
            // Transition the queued client UNDER s_unix_pair_lock (same lock the
            // connect() bail / accept() claim use) so this state write does not
            // race them; only move it if it is still CONNECTING (a client that
            // already bailed/closed keeps its DISCONNECTED state).
            spin_lock(&s_unix_pair_lock);
            if (p->client->state == UNIX_STATE_CONNECTING)
                p->client->state = UNIX_STATE_UNCONNECTED;
            spin_unlock(&s_unix_pair_lock);
            unix_wake(p->client);
            unix_put(p->client);
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

    // If this socket is still CONNECTING (queued on a listener's backlog and
    // not yet accepted), mark it dead UNDER s_unix_pair_lock so a concurrent
    // accept() (which claims under the same lock) skips it rather than pairing a
    // socket whose fd just closed.  Wake the connector so it returns instead of
    // waiting forever.  The backlog still holds a ref (unix_get in connect), so
    // `s` stays alive until accept/the listener-close drain reaps it.
    {
        int was_connecting = 0;
        spin_lock(&s_unix_pair_lock);
        if (s->state == UNIX_STATE_CONNECTING) {
            s->state = UNIX_STATE_DISCONNECTED;
            was_connecting = 1;
        }
        spin_unlock(&s_unix_pair_lock);
        if (was_connecting) unix_wake(s);
    }

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
    // Clear the symmetric back-pointer UNDER s_unix_pair_lock so it cannot race
    // accept() linking the pair: with the lock, accept's claim+link is atomic,
    // so if we observe s->peer set we observe a fully-linked pair (and clear the
    // peer's side); if we observe NULL the pair was never linked.  pin the peer
    // (refcount, lockless CAS) so it survives the wake we do AFTER unlocking.
    spin_lock(&s_unix_pair_lock);
    unix_sock_t* peer = unix_pin_peer(s);
    if (peer && peer->peer == s) {
        peer->peer  = NULL;
        peer->state = UNIX_STATE_DISCONNECTED;
    }
    spin_unlock(&s_unix_pair_lock);
    if (peer) {
        unix_wake(peer);
        unix_poll_wake(peer);
        unix_put(peer);
    }

    // Release the OWNED ref on a SOCK_DGRAM connect() default destination, if
    // any.  This is the asymmetric counterpart to the symmetric back-pointer
    // clear above: the destination never referenced us, so it could not clear
    // (or release) this link on its own close -- we took the ref in connect()
    // and must drop it here (and on reconnect).  Independent of `peer`, so a
    // socket that is both socketpair'd and connect()'d releases each correctly.
    if (s->dgram_dest) {
        unix_put(s->dgram_dest);
        s->dgram_dest = NULL;
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
    spin_lock_init(&s->lock);

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

// ── namespace string helpers selftest ─────────────────────────────────
// Audit fix (BUG-TYPE SCAN #3): sys_bind/connect/sendto used to walk the user
// sun_path as an unbounded C-string in ns_hash_str/ns_streq (read past the
// validated sockaddr into the next page -> #PF DoS).  The syscall boundary now
// copies + NUL-caps sun_path, and these helpers are capped at UNIX_PATH_MAX.
// Verify they are bounded (a full non-NUL-terminated UNIX_PATH_MAX buffer does
// not run off the end) and that equality/hashing still match C-string semantics
// for normal NUL-terminated paths.
void unix_ns_str_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    if (!ns_streq("/run/x", "/run/x")) fails++;
    if ( ns_streq("/run/x", "/run/y")) fails++;
    if (!ns_streq("", "")) fails++;
    // Hash must stop at the NUL: trailing garbage after it changes nothing.
    char a[UNIX_PATH_MAX], b[UNIX_PATH_MAX];
    __builtin_memset(a, 0, sizeof(a));
    __builtin_memset(b, 0, sizeof(b));
    a[0] = '/'; a[1] = 'x';
    b[0] = '/'; b[1] = 'x'; b[3] = 'Z'; b[4] = 'Q';   // garbage past the NUL at [2]
    if (ns_hash_str(a, 256) != ns_hash_str(b, 256)) fails++;
    // Bounded: a fully non-NUL-terminated buffer must terminate at the cap.
    __builtin_memset(a, 'A', sizeof(a));
    __builtin_memset(b, 'A', sizeof(b));
    if (!ns_streq(a, b)) fails++;            // equal through UNIX_PATH_MAX
    (void)ns_hash_str(a, 256);               // must return (capped), not run off
    b[UNIX_PATH_MAX - 1] = 'B';
    if (ns_streq(a, b)) fails++;             // differ only at the last byte
    kprintf(fails ? "[unix_ns_str] SELF-TEST FAILED\n"
                  : "[unix_ns_str] SELF-TEST PASSED (bounded hash/streq, NUL-stop)\n");
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

// ── SOCK_DGRAM connect() peer-lifetime selftest ──────────────────────
// Proves the owned-ref fix for the asymmetric connect() default destination:
// the destination must survive its OWN close as long as a connected sender
// still references it, so a later sendto cannot use-after-free it.  Also
// exercises the reconnect path's drop of the previous destination's ref.
// (unix_sock_bind/connect/sendto are the same primitives sys_bind/connect/
// sendto drive, so this mirrors the real userland reach.)
void unix_dgram_peer_selftest(void) {
    vfs_file_t* fb = unix_sock_open(SOCK_DGRAM);   // destination 1
    vfs_file_t* fc = unix_sock_open(SOCK_DGRAM);   // destination 2 (reconnect)
    vfs_file_t* fa = unix_sock_open(SOCK_DGRAM);   // sender
    if (!fb || !fc || !fa) {
        kprintf_atomic("[unix_dgram_peer] FAIL open\n");
        goto fail;
    }
    unix_sock_t* sb = (unix_sock_t*)fb->ctx;
    unix_sock_t* sc = (unix_sock_t*)fc->ctx;
    unix_sock_t* sa = (unix_sock_t*)fa->ctx;

    if (unix_sock_bind(fb, "/dgram-peer-test-a") != 0 ||
        unix_sock_bind(fc, "/dgram-peer-test-b") != 0) {
        kprintf_atomic("[unix_dgram_peer] FAIL bind\n");
        goto fail;
    }

    // connect A -> B: B gains the sender's owned ref (1 -> 2).
    if (unix_sock_connect(fa, "/dgram-peer-test-a") != 0 ||
        sa->dgram_dest != sb || sb->refcount != 2) {
        kprintf_atomic("[unix_dgram_peer] FAIL connect dest=%p refB=%lu\n",
                       (void*)sa->dgram_dest, (unsigned long)sb->refcount);
        goto fail;
    }

    // reconnect A -> C: C gains the ref (1 -> 2), B's owned ref is dropped
    // (2 -> 1, back to just its owner).
    if (unix_sock_connect(fa, "/dgram-peer-test-b") != 0 ||
        sa->dgram_dest != sc || sc->refcount != 2 || sb->refcount != 1) {
        kprintf_atomic("[unix_dgram_peer] FAIL reconnect dest=%p refB=%lu refC=%lu\n",
                       (void*)sa->dgram_dest,
                       (unsigned long)sb->refcount, (unsigned long)sc->refcount);
        goto fail;
    }

    // B is now unreferenced (only its owner): closing it frees it cleanly.
    unix_sock_close(fb); fb = NULL;

    // THE INVARIANT: close C (the live destination) while A still points at it.
    // The owner ref drops (2 -> 1) but the sender's owned ref keeps C ALIVE --
    // with the old code C would be freed here and sa->dgram_dest would dangle.
    // (fc is nulled so the fail path cannot double-close it; sc stays valid
    // because the sender's ref keeps C alive.)
    unix_sock_close(fc); fc = NULL;
    if (sc->refcount != 1) {
        kprintf_atomic("[unix_dgram_peer] FAIL dest freed under sender refC=%lu\n",
                       (unsigned long)sc->refcount);
        goto fail;
    }

    // A sendto the closed-but-referenced destination must NOT fault (no UAF);
    // the datagram queues into C and is reclaimed when C is finally freed.
    int r = unix_sock_sendto(fa, "hi", 2, NULL);
    if (r != 2) {
        kprintf_atomic("[unix_dgram_peer] FAIL sendto closed dest r=%d\n", r);
        goto fail;
    }

    // Closing A drops the last ref on C -> both freed, balanced.
    unix_sock_close(fa);
    kprintf_atomic("[unix_dgram_peer] SELF-TEST PASSED (owned dgram dest survives its close)\n");
    return;
fail:
    if (fa) unix_sock_close(fa);
    if (fb) unix_sock_close(fb);
    if (fc) unix_sock_close(fc);
    kprintf_atomic("[unix_dgram_peer] SELF-TEST FAILED\n");
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

// ── stream listen/accept backlog selftest (backlog client UAF fix) ───────
// Drives a real STREAM connect()/accept() rendezvous across two threads to
// exercise the queued-client lifetime: connect() enqueues the client on the
// listener backlog taking an OWNED ref (so a queued client that closes/bails
// cannot dangle for accept to dereference), and accept() atomically CLAIMS it
// (CONNECTING -> CONNECTED) before pairing, then drops that backlog ref.
// connect() blocks until paired, so the connector runs in its own kthread
// while the main thread accepts.  SMP/preemption is up here (smp_boot_aps ran
// before the selftest block) and the spin-waits yield, so the connector makes
// progress even were it scheduled on this CPU.
static vfs_file_t*       s_uas_client_f;     // client file, published before spawn
static volatile int      s_uas_connect_rc;   // connect() return value
static volatile uint32_t s_uas_done;         // set when the connector returns

static void uas_connector_thread(void) {
        int rc = unix_sock_connect(s_uas_client_f, "/unixtest-accept");
        s_uas_connect_rc = rc;
        __atomic_store_n(&s_uas_done, 1u, __ATOMIC_RELEASE);
}

void unix_stream_accept_selftest(void) {
        vfs_file_t* lf = unix_sock_open(SOCK_STREAM);   // listener
        vfs_file_t* cf = unix_sock_open(SOCK_STREAM);   // client
        if (!lf || !cf) {
                kprintf_atomic("[unix_stream_accept] FAIL open\n");
                if (lf) unix_sock_close(lf);
                if (cf) unix_sock_close(cf);
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED\n");
                return;
        }
        unix_sock_t* ls = (unix_sock_t*)lf->ctx;
        unix_sock_t* cs = (unix_sock_t*)cf->ctx;

        if (unix_sock_bind(lf, "/unixtest-accept") != 0 ||
            unix_sock_listen(lf, 4) != 0) {
                kprintf_atomic("[unix_stream_accept] FAIL bind/listen\n");
                unix_sock_close(lf);
                unix_sock_close(cf);
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED\n");
                return;
        }

        // Spawn the connector.  It enqueues `cf` on the listener (cs->refcount
        // 1 -> 2, the owned backlog ref), then sets cs->state = CONNECTING last,
        // wakes the listener, and blocks until cs->state changes.
        s_uas_client_f   = cf;
        s_uas_connect_rc = 0x7fffffff;
        __atomic_store_n(&s_uas_done, 0u, __ATOMIC_RELEASE);
        task_t* t = task_create_kthread(uas_connector_thread, pid_alloc());
        if (!t) {
                kprintf_atomic("[unix_stream_accept] FAIL spawn\n");
                unix_sock_close(lf);
                unix_sock_close(cf);
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED\n");
                return;
        }
        sched_add(t);

        // Wait (bounded, yielding) for the enqueue to fully complete.  cs->state
        // is set to CONNECTING LAST in connect() -- after unix_get and the
        // backlog link/count -- so observing it guarantees refcount==2 and
        // backlog_count==1 are already visible (x86 TSO: stores in order).
        // From here the connector may block holding the backlog ref, so a
        // failure prints and returns WITHOUT closing (avoids racing it); the
        // bug is the regression we are catching.
        uint64_t spins = 0;
        while (__atomic_load_n(&cs->state, __ATOMIC_ACQUIRE) != UNIX_STATE_CONNECTING) {
                if (++spins > 50000000ull) {
                        kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED (no enqueue)\n");
                        return;
                }
                sched_yield();
        }
        if (cs->refcount != 2 ||
            __atomic_load_n(&ls->backlog_count, __ATOMIC_ACQUIRE) != 1) {
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED (enqueue ref=%lu bk=%lu)\n",
                               (unsigned long)cs->refcount,
                               (unsigned long)__atomic_load_n(&ls->backlog_count, __ATOMIC_ACQUIRE));
                return;
        }

        // Accept: claims the client (CONNECTING -> CONNECTED), builds the server
        // side, links the symmetric peers, wakes the connector, and drops the
        // backlog ref (2 -> 1).
        vfs_file_t* sf = unix_sock_accept(lf);
        if (!sf) {
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED (accept NULL)\n");
                return;
        }
        unix_sock_t* ss = (unix_sock_t*)sf->ctx;

        // Wait (bounded) for the connector to observe the pairing and return.
        spins = 0;
        while (__atomic_load_n(&s_uas_done, __ATOMIC_ACQUIRE) == 0) {
                if (++spins > 50000000ull) {
                        kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED (connector stuck)\n");
                        unix_sock_close(sf);
                        return;
                }
                sched_yield();
        }

        // Verify: connect() succeeded, both ends CONNECTED, symmetric peers
        // linked, backlog drained, and the owned ref dropped back to the owner.
        if (s_uas_connect_rc != 0 ||
            cs->state != UNIX_STATE_CONNECTED || ss->state != UNIX_STATE_CONNECTED ||
            ss->peer != cs || cs->peer != ss ||
            cs->refcount != 1 ||
            __atomic_load_n(&ls->backlog_count, __ATOMIC_ACQUIRE) != 0) {
                kprintf_atomic("[unix_stream_accept] SELF-TEST FAILED (pair rc=%d cstate=%u "
                               "sstate=%u peers %d/%d ref=%lu bk=%lu)\n",
                               s_uas_connect_rc, (unsigned)cs->state, (unsigned)ss->state,
                               ss->peer == cs, cs->peer == ss,
                               (unsigned long)cs->refcount,
                               (unsigned long)__atomic_load_n(&ls->backlog_count, __ATOMIC_ACQUIRE));
                unix_sock_close(sf);
                return;
        }

        // Teardown: the connector has returned (it never touches cf after
        // setting s_uas_done).  Server close clears the client's symmetric
        // back-pointer; all three free without fault and the refcounts balance.
        unix_sock_close(sf);
        unix_sock_close(cf);
        unix_sock_close(lf);
        kprintf_atomic("[unix_stream_accept] PASS (backlog owned-ref + atomic accept claim)\n");
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
    // pend in backlog_head).  Loop so we can skip+reap a queued client
    // that already closed/bailed (its CAS claim below fails).
    for (;;) {
        WAIT_EVENT_HOOK(&listener->waitq,
                        listener->backlog_head != NULL
                            || listener->state != UNIX_STATE_LISTENING,
                        if (signal_has_actionable(&g_current->sigstate))
                            return NULL;);
        if (listener->state != UNIX_STATE_LISTENING) return NULL;

        // Dequeue the first pending connection UNDER the listener's per-socket
        // lock so a concurrent connect() push or a second accept() pop cannot
        // tear the singly-linked list or double-pop (double-free) the same node.
        // Re-check the head inside the lock (the wait condition was tested
        // unlocked, and another accepter may have drained it).
        spin_lock(&listener->lock);
        unix_pending_t* pend;
        FIFO_DEQUEUE_HEAD(listener->backlog_head, listener->backlog_tail,
                          listener->backlog_count, pend);
        spin_unlock(&listener->lock);
        if (!pend) continue;   // empty (spurious wake / lost the race) -> re-wait

        unix_sock_t* client = pend->client;   // owns one ref (unix_get in connect)
        kfree(pend);
        if (!client) continue;

        // Lockless pre-check: skip a client that already bailed/closed so we do
        // not allocate a server for it.  The authoritative claim is under the
        // lock below; this only avoids wasted work in the common bailed case.
        if (__atomic_load_n(&client->state, __ATOMIC_ACQUIRE)
                != UNIX_STATE_CONNECTING) {
            unix_put(client);   // drop the backlog ref (reaping it)
            continue;
        }

        // Create the server-side socket that pairs with the client (outside the
        // pair lock -- unix_sock_open allocates).
        vfs_file_t* server_f = unix_sock_open(SOCK_STREAM);
        if (!server_f) {
            // Reject the client: wake the connector so it returns -ECONNREFUSED.
            spin_lock(&s_unix_pair_lock);
            if (client->state == UNIX_STATE_CONNECTING)
                client->state = UNIX_STATE_UNCONNECTED;
            spin_unlock(&s_unix_pair_lock);
            unix_wake(client);
            unix_put(client);   // drop the backlog ref
            return NULL;
        }

        unix_sock_t* server = (unix_sock_t*)server_f->ctx;

        // Claim + link ATOMICALLY under s_unix_pair_lock.  If the client is
        // still CONNECTING we win: publish CONNECTED and link both peer pointers
        // as one indivisible unit, so a concurrent close() on the client (which
        // takes the same lock) observes either an unlinked CONNECTING client (it
        // bails, we fail the claim) or a fully-linked CONNECTED pair (it clears
        // server->peer) -- never the half-linked state that dangled server->peer.
        int claimed = 0;
        spin_lock(&s_unix_pair_lock);
        if (client->state == UNIX_STATE_CONNECTING) {
            client->state    = UNIX_STATE_CONNECTED;
            server->peer     = client;
            client->peer     = server;
            server->state    = UNIX_STATE_CONNECTED;
            // Trusted peer pids: the server's peer is the connecting process
            // (recorded on the client during connect()); the client's peer is
            // the accepting process (g_current).
            server->peer_pid = client->peer_pid;
            client->peer_pid = g_current->pid;
            claimed = 1;
        }
        spin_unlock(&s_unix_pair_lock);

        if (!claimed) {
            // The client bailed/closed while we allocated the server -> tear the
            // unused server down and try the next pending entry.
            unix_sock_close(server_f);
            unix_put(client);   // drop the backlog ref (reaping it)
            continue;
        }

        // Wake the client blocked in connect().
        unix_wake(client);
        // Drop the backlog ref: the client is now kept alive by its own fd, and
        // the symmetric peer relationship (cleared on either side's close).
        unix_put(client);

        return server_f;
    }
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
        // SOCK_DGRAM: remember the default destination.  This is an ASYMMETRIC
        // link -- `target` has no back-pointer to us, so its close() cannot
        // clear our cached pointer (unlike a symmetric stream/socketpair peer).
        // Hold an OWNED strong ref so `target` cannot be freed out from under a
        // later send; without it dgram_dest would dangle to freed memory the
        // moment the destination closes (a use-after-free reachable by a plain
        // sendto after the peer's close).  `target` is alive here (we are in
        // the ns_find reader section), and unix_get publishes a ref that keeps
        // it alive past rcu_read_unlock.  A re-connect drops the PREVIOUS
        // destination's ref; publish the new pointer BEFORE dropping the old so
        // a concurrent send observes either the ref-held new dest or the old
        // one still kept alive for that send's own grace period.
        unix_sock_t* old = s->dgram_dest;
        unix_get(target);
        s->dgram_dest = target;
        s->state      = UNIX_STATE_CONNECTED;
        rcu_read_unlock();
        if (old) unix_put(old);
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

    // Enqueue ourselves.  Take an OWNED ref so the backlog entry keeps `s`
    // alive even if our fd closes while we sit queued (the listener's
    // accept()/close-drain drops this ref).  Without it, a client that bails
    // (-EINTR below) and closes leaves pend->client dangling for accept to
    // dereference -- a use-after-free.  Mirrors the F58 dgram_dest owned ref.
    unix_pending_t* pend = kmalloc(sizeof(unix_pending_t));   // alloc OUTSIDE the lock
    if (!pend) { rcu_read_unlock(); return -ENOMEM; }
    pend->next   = NULL;
    unix_get(s);
    pend->client = s;

    // Link onto the listener backlog under its per-socket lock so a concurrent
    // accept() pop, a second connect() push, and the count++ cannot tear the
    // list or lose the count.  Re-check LISTENING + the backlog limit under the
    // lock (a racing fill could have crossed it since the unlocked pre-check).
    spin_lock(&target->lock);
    if (target->state != UNIX_STATE_LISTENING ||
        target->backlog_count >= target->backlog_max) {
        spin_unlock(&target->lock);
        unix_put(s);          // undo the backlog ref we just took
        kfree(pend);
        rcu_read_unlock();
        return -ECONNREFUSED;
    }
    FIFO_ENQUEUE_TAIL(target->backlog_head, target->backlog_tail, target->backlog_count, pend);
    s->state = UNIX_STATE_CONNECTING;
    spin_unlock(&target->lock);

    // Wake the listener if it's blocked in accept() or poll().  OUTSIDE the lock
    // -- wait_queue_wake_all touches rq_lock, which must never nest under a leaf
    // data lock.
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
                    if (signal_has_actionable(&g_current->sigstate)) {
                        // Claim the bail UNDER s_unix_pair_lock (same lock accept
                        // claims with, so the two cannot interleave).  If we are
                        // still CONNECTING we win -> mark DISCONNECTED and return
                        // -EINTR; accept's locked claim then fails and reaps us.
                        // If accept already connected us, we are CONNECTED here ->
                        // fall through; the loop re-checks state == CONNECTED.
                        int bailed = 0;
                        spin_lock(&s_unix_pair_lock);
                        if (s->state == UNIX_STATE_CONNECTING) {
                            s->state = UNIX_STATE_DISCONNECTED;
                            bailed = 1;
                        }
                        spin_unlock(&s_unix_pair_lock);
                        if (bailed) return -EINTR;
                    });

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
        uint8_t kbuf[512];   // bounce chunk: the user->kernel copy stays OUTSIDE
                             // the per-socket lock; bounds stack + lock-hold.

        while (total < len) {
            // If peer closed, EPIPE.  (close() clears s->peer + sets state.)
            if (!s->peer || s->state == UNIX_STATE_DISCONNECTED) {
                ret = total > 0 ? (int)total : -EPIPE;
                goto stream_out;
            }

            // Bounce a bounded chunk of the user buffer into kbuf OUTSIDE the
            // lock, then commit it into the peer's ring under the peer's lock.
            uint32_t chunk = len - total;
            if (chunk > sizeof(kbuf)) chunk = (uint32_t)sizeof(kbuf);
            __builtin_memcpy(kbuf, (const uint8_t*)buf + total, chunk);
            uint32_t wrote = cbuf_write_locked(peer, kbuf, chunk);
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

        // Drain up to `len` bytes from our ring into the user buffer, bouncing
        // through a kernel chunk so the ring copy + buf_count are under our lock
        // but the user copy is OUTSIDE it.  A stream read returns whatever is
        // available now (it does not wait to fill `len`), so stop when the ring
        // drains (cbuf_read_locked returns 0).
        uint8_t kbuf[512];
        uint32_t got = 0;
        while (got < len) {
            uint32_t chunk = len - got;
            if (chunk > sizeof(kbuf)) chunk = (uint32_t)sizeof(kbuf);
            uint32_t r = cbuf_read_locked(s, kbuf, chunk);
            if (r == 0) break;
            __builtin_memcpy((uint8_t*)buf + got, kbuf, r);
            got += r;
        }

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

    // Dequeue under the per-socket lock (a 2nd receiver or a concurrent sendto
    // enqueue must not tear the list / double-pop).  Re-check the head inside.
    spin_lock(&s->lock);
    unix_dgram_t* msg;
    FIFO_DEQUEUE_HEAD(s->dgram_head, s->dgram_tail, s->dgram_count, msg);
    spin_unlock(&s->lock);
    if (!msg) return 0;   // lost the race / spurious wake -> nothing to read

    // msg is unlinked and solely ours now -> copy to the user + free OUTSIDE the
    // lock (the user write must never fault under the spinlock).
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
        // No explicit destination: use the connect() default if set (owned
        // strong ref -> never dangling), else the symmetric socketpair peer
        // (kept alive by its back-pointer clear).  Both are memory-safe to
        // deref within this rcu_read_lock section; the cached-but-unreferenced
        // dangling case that used to live here is gone (see dgram_dest).
        target = s->dgram_dest ? s->dgram_dest : s->peer;
    }
    if (!target) { rcu_read_unlock(); return -ECONNREFUSED; }
    if (target->type != SOCK_DGRAM) { rcu_read_unlock(); return -ECONNREFUSED; }

    // Allocate message (header + data in one allocation).
    unix_dgram_t* msg = kmalloc(sizeof(unix_dgram_t) + len);
    if (!msg) { rcu_read_unlock(); return -ENOMEM; }
    msg->next = NULL;
    msg->len  = len;

    __builtin_memcpy(UNIX_DGRAM_DATA(msg), buf, len);

    // Enqueue into target's receive queue under its per-socket lock so a
    // concurrent recv() dequeue or a second sendto() enqueue cannot tear the
    // singly-linked list or lose the count.  The kmalloc + the user-data memcpy
    // above and the wakes below stay OUTSIDE the lock (the memcpy reads user
    // memory, which must never fault under a preempt-disabled spinlock; the
    // wakes touch rq_lock).
    spin_lock(&target->lock);
    FIFO_ENQUEUE_TAIL(target->dgram_head, target->dgram_tail, target->dgram_count, msg);
    spin_unlock(&target->lock);

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

    // Dup the file description and stamp attenuated rights (a refcount bump --
    // done OUTSIDE the lock).
    vfs_file_t* dup = vfs_dup(file);
    if (!dup) { unix_put(peer); return -ENOMEM; }
    dup->rights = rights;

    // Enqueue under the peer's per-socket lock (the peer OWNS the ancillary
    // queue) so a concurrent recvfd dequeue or a 2nd sendfd cannot lose the
    // count or double-occupy a slot.  Re-check the limit under the lock.
    spin_lock(&peer->lock);
    if (anc->count >= UNIX_ANCILLARY_MAX) {
        spin_unlock(&peer->lock);
        vfs_close(dup);          // undo the dup we just took
        unix_put(peer);
        return -ENOBUFS;
    }
    uint8_t idx = anc->tail;
    anc->files[idx]  = dup;
    anc->rights[idx] = rights;
    anc->tail = (anc->tail + 1) % UNIX_ANCILLARY_MAX;
    anc->count++;
    spin_unlock(&peer->lock);

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

    // Dequeue under the owner's per-socket lock, re-checking count inside so a
    // 2nd recvfd or a concurrent sendfd cannot double-take the same slot.
    spin_lock(&s->lock);
    vfs_file_t* file = NULL;
    if (anc->count != 0) {
        uint8_t idx = anc->head;
        file = anc->files[idx];
        anc->files[idx] = NULL;
        anc->head = (anc->head + 1) % UNIX_ANCILLARY_MAX;
        anc->count--;
    }
    spin_unlock(&s->lock);
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

    // Dequeue under the owner's per-socket lock, re-checking count inside (the
    // wait condition was tested unlocked; another recvfd may have drained it).
    spin_lock(&s->lock);
    vfs_file_t* file = NULL;
    if (anc->count != 0) {
        uint8_t idx = anc->head;
        file = anc->files[idx];
        anc->files[idx] = NULL;
        anc->head = (anc->head + 1) % UNIX_ANCILLARY_MAX;
        anc->count--;
    }
    spin_unlock(&s->lock);
    return file;
}
