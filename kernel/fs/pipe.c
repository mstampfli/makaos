#include "pipe.h"
#include "kheap.h"
#include "sched.h"
#include "signal.h"
#include "process.h"
#include "errno.h"
#include "common.h"
#include "wait.h"
#include "rcu.h"

// ── Pipe VFS callbacks ────────────────────────────────────────────────────

static int64_t pipe_read(vfs_file_t* self, void* buf, uint64_t len) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;

    // Canonical Phase 9-6 pattern.  Outer while provides phase 1
    // (quick check), task_we_add provides phase 2, the second check
    // under registration provides phase 3, sched_sleep is phase 4.
    // task_we_remove runs on every exit path (continue, break,
    // return-EINTR, return-count).
    while (total < len) {
        if (p->count == 0) {
            if (p->writer_refs == 0) {
                // EOF: wake writers (there shouldn't be any, but a
                // racing close might have left SIGPIPE waiters) and
                // return what we've got (possibly 0).
                if (p->write_file)
                    wait_queue_wake_all(p->write_file->waitq);
                return (int64_t)total;
            }

            WAIT_EVENT_HOOK(self->waitq,
                            p->count != 0 || p->writer_refs == 0,
                            if (signal_has_actionable(&g_current->sigstate)) {
                                if (total > 0) return (int64_t)total;
                                return (int64_t)-EINTR;
                            });
            continue;
        }
        dst[total++] = p->buf[p->head];
        p->head = (p->head + 1) & (PIPE_BUF_SIZE - 1);
        p->count--;
    }
    // Wake any writer waiting for space.  Commit order: the drain
    // above happens BEFORE the wake_all xchg, so any writer that
    // wakes sees the new p->count.
    if (p->write_file) wait_queue_wake_all(p->write_file->waitq);
    return (int64_t)total;
}

// deliver_sigpipe — send SIGPIPE to g_current unless it's blocked or ignored.
// Returns 1 if signal was sent (caller should return -EPIPE),
//         0 if suppressed (SIG_IGN or blocked — caller still returns -EPIPE).
static int deliver_sigpipe(void) {
    if (!g_current) return 0;
    sigstate_t* ss = &g_current->sigstate;
    uint32_t bit = 1u << (SIGPIPE - 1);
    // Suppressed: blocked mask or SIG_IGN handler.
    if (ss->blocked & bit) return 0;
    if (ss->handlers[SIGPIPE].sa_handler == (uint64_t)SIG_IGN) return 0;
    serial_puts_dbg("[pipe] SIGPIPE → pid=");
    serial_hex_dbg((uint64_t)g_current->pid);
    signal_send(g_current, SIGPIPE);
    return 1;
}

static int64_t pipe_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;

    // Read end already closed before we started — SIGPIPE + EPIPE immediately.
    if (p->reader_refs == 0) {
        deliver_sigpipe();
        return (int64_t)-EPIPE;
    }

    while (total < len) {
        // Canonical Phase 9-6 pattern.  Phase 1 is the top-of-loop
        // full check; phase 2 is task_we_add; phase 3 is the
        // re-check under the registration; phase 4 is sched_sleep;
        // every exit path removes the entry first.
        if (p->count == PIPE_BUF_SIZE) {
            if (p->reader_refs == 0) {
                deliver_sigpipe();
                return total ? (int64_t)total : (int64_t)-EPIPE;
            }

            WAIT_EVENT_HOOK(self->waitq,
                            p->count != PIPE_BUF_SIZE || p->reader_refs == 0,
                            if (signal_has_actionable(&g_current->sigstate)) {
                                if (total > 0) return (int64_t)total;
                                return (int64_t)-EINTR;
                            });
            continue;
        }
        p->buf[p->tail] = src[total++];
        p->tail = (p->tail + 1) & (PIPE_BUF_SIZE - 1);
        p->count++;
    }

    // Commit all pushes BEFORE wake_all's xchg — any reader drained
    // from the queue after this xchg will observe the new p->count
    // thanks to the ACQ_REL on the xchg pairing with the reader's
    // subsequent lock acquire in sched_sleep.
    if (p->read_file) {
        wait_queue_wake_all(p->read_file->waitq);
    }

    return (int64_t)total;
}

static void pipe_read_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->reader_refs > 0) p->reader_refs--;
    // Wake any blocked writers so they can return -EPIPE.
    if (p->reader_refs == 0 && p->write_file) {
        wait_queue_wake_all(p->write_file->waitq);
    }
    if (p->reader_refs == 0 && p->writer_refs == 0) kfree(p);
    kfree(self);
}

static void pipe_write_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->writer_refs > 0) p->writer_refs--;
    // Wake any sleeping reader so they see EOF immediately.
    if (p->writer_refs == 0 && p->read_file) {
        wait_queue_wake_all(p->read_file->waitq);
    }
    if (p->reader_refs == 0 && p->writer_refs == 0) kfree(p);
    kfree(self);
}

// poll: check readiness without blocking.
static int pipe_read_poll(vfs_file_t* self, int events) {
    (void)events;
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // Readable if data available OR write end closed (EOF).
    return (p->count > 0 || p->writer_refs == 0) ? 1 : 0;
}

static int pipe_write_poll(vfs_file_t* self, int events) {
    (void)events;
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // Writable if space available and read end still open.
    return (p->count < PIPE_BUF_SIZE && p->reader_refs > 0) ? 1 : 0;
}

// ── pipe_ioctl ────────────────────────────────────────────────────────────
// FIONREAD = _IOR('T', 0x1b, int): bytes available to read right now.
// Consumers size a read by it — swaybar's status reader does
// ioctl(read_fd, FIONREAD, &n) then read(read_fd, buf, n).  Without a pipe
// ioctl handler the call fell through sys_ioctl to the controlling-tty
// fallback, which never wrote the user int, so `n` stayed UNINITIALIZED:
// swaybar then read a garbage-sized buffer, producing a multi-KB status
// string that rendered to ~175K cairo commands and blew up the recording
// surface's bbtree (intermittent stack-overflow / heap-exhaustion crash).
// Answer FIONREAD with the queued byte count; anything else is -ENOTTY (a
// pipe is not a terminal — this also makes isatty() correctly return false).
#define PIPE_FIONREAD 0x541b
static int64_t pipe_ioctl(vfs_file_t* self, uint64_t request, uint64_t arg) {
    extern int copy_to_user(void* dst_u, const void* src, uint64_t len);
    if ((uint32_t)request == PIPE_FIONREAD) {
        pipe_buf_t* p = (pipe_buf_t*)self->ctx;
        int navail = p ? (int)p->count : 0;
        if (copy_to_user((void*)arg, &navail, sizeof(navail)) != 0)
            return -EFAULT;
        return 0;
    }
    return -ENOTTY;
}

// ── pipe_create ───────────────────────────────────────────────────────────

int pipe_create(vfs_file_t** read_end, vfs_file_t** write_end) {
    pipe_buf_t* p = kmalloc(sizeof(pipe_buf_t));
    if (!p) return -ENOMEM;

    __builtin_memset(p->buf, 0, PIPE_BUF_SIZE);
    p->head = p->tail = p->count = 0;
    p->writer_refs = 1;
    p->reader_refs = 1;

    vfs_file_t* r = kmalloc(sizeof(vfs_file_t));
    vfs_file_t* w = kmalloc(sizeof(vfs_file_t));
    if (!r || !w) {
        if (r) kfree(r);
        if (w) kfree(w);
        kfree(p);
        return -ENOMEM;
    }
    __builtin_memset(r, 0, sizeof(*r));
    __builtin_memset(w, 0, sizeof(*w));

    r->read        = pipe_read;
    r->write       = NULL;
    r->seek        = NULL;
    r->close       = pipe_read_close;
    r->poll           = pipe_read_poll;
    r->ioctl          = pipe_ioctl;
    r->ctx            = p;
    r->waitq           = &r->_waitq; wait_queue_init(r->waitq);
    r->secondary_waitq = NULL;
    r->flags          = 0;
    r->refcount    = 1;
    r->rights      = 0;
    r->path[0]     = '\0';

    w->read        = NULL;
    w->write       = pipe_write;
    w->seek        = NULL;
    w->close       = pipe_write_close;
    w->poll           = pipe_write_poll;
    w->ioctl          = NULL;
    w->ctx            = p;
    w->waitq           = &w->_waitq; wait_queue_init(w->waitq);
    w->secondary_waitq = NULL;
    w->flags          = 0;
    w->refcount    = 1;
    w->rights      = 0;
    w->path[0]     = '\0';

    p->read_file  = r;
    p->write_file = w;

    *read_end  = r;
    *write_end = w;
    return 0;
}
