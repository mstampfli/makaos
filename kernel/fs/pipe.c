#include "pipe.h"
#include "kheap.h"
#include "sched.h"
#include "signal.h"
#include "process.h"
#include "errno.h"
#include "common.h"
#include "wait.h"
#include "rcu.h"

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif

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

            // O_NONBLOCK: an empty pipe with a live writer must not block.
            // Return whatever we've drained so far, or -EAGAIN if nothing.
            // Without this a non-blocking reader (e.g. swaybar's getline on
            // its status_command pipe, set O_NONBLOCK via F_SETFL) blocks
            // forever instead of seeing EAGAIN, freezing its event loop.
            if (self->flags & O_NONBLOCK)
                return total > 0 ? (int64_t)total : (int64_t)-EAGAIN;

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

// Drop one of the pipe's two open ends.  Returns 1 ONLY for the close that
// drives open_ends to 0 (the last end) -- that caller owns the teardown.  The
// ACQ_REL ordering makes every prior close hook's peer-wake happen-before this
// caller's free, so the free can never race a concurrent wake_all of a peer
// waitq.  Pure (operates on *open_ends) -> unit-tested below.
static int pipe_last_end_release(uint32_t* open_ends) {
    return __atomic_sub_fetch(open_ends, 1u, __ATOMIC_ACQ_REL) == 0;
}

// Free the whole pipe -- both end vfs_file_t and the shared buffer -- in one
// place, so the read and write files share the buffer's lifetime (like the
// F14 AF_UNIX file<->sock invariant) and a close hook can always wake the peer
// end's waitq without it having been freed underneath us.
static void pipe_destroy(pipe_buf_t* p) {
    if (p->read_file)  kfree(p->read_file);
    if (p->write_file) kfree(p->write_file);
    kfree(p);
}

static void pipe_read_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->reader_refs > 0) p->reader_refs--;
    // Wake any blocked writers so they can return -EPIPE.  p->write_file is
    // alive: it is only freed by the last-end close (pipe_destroy) below.
    if (p->reader_refs == 0 && p->write_file) {
        wait_queue_wake_all(p->write_file->waitq);
    }
    // Drop this end; the last end to close frees p + both files (NOT a bare
    // kfree(self) -- that would double-free p and leave the peer file dangling
    // for the other hook's wake).
    if (pipe_last_end_release(&p->open_ends)) pipe_destroy(p);
}

static void pipe_write_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->writer_refs > 0) p->writer_refs--;
    // Wake any sleeping reader so they see EOF immediately.  p->read_file is
    // alive until the last-end close frees it.
    if (p->writer_refs == 0 && p->read_file) {
        wait_queue_wake_all(p->read_file->waitq);
    }
    if (pipe_last_end_release(&p->open_ends)) pipe_destroy(p);
}

// poll: check readiness without blocking.
// poll(2) revents bit (mirrors syscall.h POLLHUP); the vfs poll hook is asked
// for one condition at a time via f->poll(f, <bit>).
#define PIPE_POLLHUP 0x0010

static int pipe_read_poll(vfs_file_t* self, int events) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // POLLHUP probe (fd_has_hup -> f->poll(f, POLLHUP)): a pipe read end has
    // hung up ONLY once every writer has closed.  A pipe still holding buffered
    // data with a LIVE writer is not hung up.  The old code ignored `events`
    // and always answered the readability question (count>0 || writers==0), so
    // a POLLHUP probe returned true whenever data was queued -> the poll layer
    // OR'd in a spurious POLLHUP, and swaybar treated its still-running status
    // command as crashed and rendered "[error reading from status command]".
    if (events & PIPE_POLLHUP)
        return (p && p->writer_refs == 0) ? 1 : 0;
    // POLLIN / default: readable if data is available OR the write end closed.
    return (p && (p->count > 0 || p->writer_refs == 0)) ? 1 : 0;
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
    p->open_ends   = 2;   // read end + write end; last close frees p + both files

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

// ── pipe_last_end_release selftest ────────────────────────────────────────
// Deterministic check that the two pipe-end closes yield EXACTLY ONE "last"
// release -> exactly one free, never two (the double-free this fixes).  Mirrors
// the unix_refcount_tryget single-owner-transition test (F15).
void pipe_refcount_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    uint32_t oe = 2;                          // both ends open

    int r1 = pipe_last_end_release(&oe);      // first close: 2 -> 1, NOT last
    if (r1 != 0 || oe != 1) {
        kprintf("[pipe_refcount] FAIL first r=%d oe=%lu\n", r1, (unsigned long)oe);
        fails++;
    }
    int r2 = pipe_last_end_release(&oe);      // second close: 1 -> 0, IS last
    if (r2 != 1 || oe != 0) {
        kprintf("[pipe_refcount] FAIL second r=%d oe=%lu\n", r2, (unsigned long)oe);
        fails++;
    }
    // Exactly one of the two returned "last" (r2 only) -> exactly one free.
    if (r1 + r2 != 1) {
        kprintf("[pipe_refcount] FAIL not-exactly-one-last r1=%d r2=%d\n", r1, r2);
        fails++;
    }
    kprintf(fails ? "[pipe_refcount] SELF-TEST FAILED\n"
                  : "[pipe_refcount] SELF-TEST PASSED (single last-end release, no double free)\n");
}
