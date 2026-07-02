#include "pipe.h"
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
#include "kheap.h"
#include "uaccess.h"   // copy_to_user (shared decl)
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
        spin_lock(&p->lock);
        if (p->count == 0) {
            int writer_gone = (p->writer_refs == 0);
            spin_unlock(&p->lock);
            if (writer_gone) {
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
        // Drain the available bytes into the user buffer UNDER the lock so the
        // head/count RMW cannot race a concurrent reader or writer.  dst is a
        // sys_read buffer prefaulted by user_buf_prefault, so the copy cannot
        // fault under the spinlock (same convention as pty_master_drain).
        while (total < len && p->count > 0) {
            dst[total++] = p->buf[p->head];
            p->head = (p->head + 1) & (PIPE_BUF_SIZE - 1);
            p->count--;
        }
        spin_unlock(&p->lock);
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

    // Read end already closed before we started -- SIGPIPE + EPIPE immediately.
    spin_lock(&p->lock);
    int reader_gone0 = (p->reader_refs == 0);
    spin_unlock(&p->lock);
    if (reader_gone0) {
        deliver_sigpipe();
        return (int64_t)-EPIPE;
    }

    while (total < len) {
        // Phase-1 check under the lock: is the read end gone, or the ring full?
        spin_lock(&p->lock);
        int reader_gone = (p->reader_refs == 0);
        int full        = (p->count == PIPE_BUF_SIZE);
        spin_unlock(&p->lock);
        if (reader_gone) {
            deliver_sigpipe();
            return total ? (int64_t)total : (int64_t)-EPIPE;
        }
        if (full) {
            WAIT_EVENT_HOOK(self->waitq,
                            p->count != PIPE_BUF_SIZE || p->reader_refs == 0,
                            if (signal_has_actionable(&g_current->sigstate)) {
                                if (total > 0) return (int64_t)total;
                                return (int64_t)-EINTR;
                            });
            continue;
        }

        // Copy a chunk from the USER source into a kernel bounce buffer with
        // the lock DROPPED: sys_write validates but does NOT prefault the
        // source (a write READS the buffer, so absent pages are demand-paged
        // on access), so src[] may fault and must never be touched under the
        // spinlock.  tmp is 256 bytes -- well within the 8KiB kstack budget.
        uint8_t tmp[256];
        uint64_t want  = len - total;
        uint32_t chunk = want < sizeof(tmp) ? (uint32_t)want : (uint32_t)sizeof(tmp);
        for (uint32_t i = 0; i < chunk; i++)
            tmp[i] = src[total + i];

        // Push as much of the chunk as fits into the ring UNDER the lock.  A
        // concurrent writer may have refilled the ring while the lock was
        // dropped, so pushed can be < chunk (even 0); the unpushed bytes are
        // re-read next iteration.  The p->count < PIPE_BUF_SIZE guard makes an
        // overflow impossible regardless of races.
        spin_lock(&p->lock);
        uint32_t pushed = 0;
        while (pushed < chunk && p->count < PIPE_BUF_SIZE) {
            p->buf[p->tail] = tmp[pushed++];
            p->tail = (p->tail + 1) & (PIPE_BUF_SIZE - 1);
            p->count++;
        }
        spin_unlock(&p->lock);
        total += pushed;

        // Wake readers as soon as data lands (commit the pushes BEFORE the
        // wake_all xchg so a reader drained after the xchg sees the new
        // count).  Waking per chunk -- not only at the end -- means a reader
        // blocked on an empty pipe is woken even when this writer then blocks
        // on a full ring before finishing the whole len.
        if (pushed && p->read_file)
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
    // Clear the reader flag under the ring lock so the concurrent pipe_write
    // path (which reads reader_refs under the same lock) sees a consistent
    // value, then drop the lock before the wake (which can schedule) and the
    // free (which would free the lock itself).
    spin_lock(&p->lock);
    if (p->reader_refs > 0) p->reader_refs--;
    int wake_writers = (p->reader_refs == 0);
    spin_unlock(&p->lock);
    // Wake any blocked writers so they can return -EPIPE.  p->write_file is
    // alive: it is only freed by the last-end close (pipe_destroy) below.
    if (wake_writers && p->write_file) {
        wait_queue_wake_all(p->write_file->waitq);
    }
    // Drop this end; the last end to close frees p + both files (NOT a bare
    // kfree(self) -- that would double-free p and leave the peer file dangling
    // for the other hook's wake).
    if (pipe_last_end_release(&p->open_ends)) pipe_destroy(p);
}

static void pipe_write_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // Clear the writer flag under the ring lock so the concurrent pipe_read
    // path (which reads writer_refs under the same lock) sees a consistent
    // value; drop the lock before the wake and the free.
    spin_lock(&p->lock);
    if (p->writer_refs > 0) p->writer_refs--;
    int wake_readers = (p->writer_refs == 0);
    spin_unlock(&p->lock);
    // Wake any sleeping reader so they see EOF immediately.  p->read_file is
    // alive until the last-end close frees it.
    if (wake_readers && p->read_file) {
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

    spin_lock_init(&p->lock);
    __builtin_memset(p->buf, 0, PIPE_BUF_SIZE);
    p->head = p->tail = p->count = 0;
    p->writer_refs = 1;
    p->reader_refs = 1;
    p->open_ends   = 2;   // read end + write end; last close frees p + both files

    vfs_file_t* r = vfs_alloc_file();   // each: zeroed, waitq wired, refcount=1
    vfs_file_t* w = vfs_alloc_file();
    if (!r || !w) {
        if (r) kfree(r);
        if (w) kfree(w);
        kfree(p);
        return -ENOMEM;
    }
    r->read  = pipe_read;
    r->close = pipe_read_close;
    r->poll  = pipe_read_poll;
    r->ioctl = pipe_ioctl;
    r->ctx   = p;

    w->write = pipe_write;
    w->close = pipe_write_close;
    w->poll  = pipe_write_poll;
    w->ctx   = p;

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
        kprintf_atomic("[pipe_refcount] FAIL first r=%d oe=%lu\n", r1, (unsigned long)oe);
        fails++;
    }
    int r2 = pipe_last_end_release(&oe);      // second close: 1 -> 0, IS last
    if (r2 != 1 || oe != 0) {
        kprintf_atomic("[pipe_refcount] FAIL second r=%d oe=%lu\n", r2, (unsigned long)oe);
        fails++;
    }
    // Exactly one of the two returned "last" (r2 only) -> exactly one free.
    if (r1 + r2 != 1) {
        kprintf_atomic("[pipe_refcount] FAIL not-exactly-one-last r1=%d r2=%d\n", r1, r2);
        fails++;
    }
    kprintf_atomic(fails ? "[pipe_refcount] SELF-TEST FAILED\n"
                  : "[pipe_refcount] SELF-TEST PASSED (single last-end release, no double free)\n");
}

// ── Concurrent read/write race selftest ───────────────────────────────────
// Drives the F93 fix: a writer and a reader run on (scheduler-spread) CPUs
// against ONE shared pipe, as fork/thread siblings sharing the fd would.  The
// writer emits a known byte sequence (byte at stream offset k == k & 0xFF);
// the reader verifies every byte it receives equals its expected offset value
// AND that the total received equals what was sent.  Without the ring lock the
// non-atomic count--/count++ (and head/tail RMW) lose or duplicate bytes, so
// the reader sees a value mismatch or a short total.  The writer closes the
// write end after the last byte, so the reader always terminates on EOF and
// the test cannot hang even when the (buggy) path drops bytes.
#define PR_TOTAL  (64u * 1024u)   // 64 KiB: wraps the 4 KiB ring 16 times
#define PR_CHUNK  200u

static vfs_file_t*       s_pr_r;
static vfs_file_t*       s_pr_w;
static volatile uint32_t s_pr_done;
static volatile uint32_t s_pr_fail;
static volatile uint32_t s_pr_read_total;

static void pr_writer_thread(void) {
    uint64_t off = 0;
    while (off < PR_TOTAL) {
        uint8_t chunk[PR_CHUNK];
        uint64_t want = PR_TOTAL - off;
        uint32_t n = want < PR_CHUNK ? (uint32_t)want : PR_CHUNK;
        for (uint32_t i = 0; i < n; i++)
            chunk[i] = (uint8_t)((off + i) & 0xFFu);
        int64_t w = s_pr_w->write(s_pr_w, chunk, n);
        if (w <= 0) { __atomic_store_n(&s_pr_fail, 1u, __ATOMIC_RELEASE); break; }
        off += (uint64_t)w;
    }
    s_pr_w->close(s_pr_w);   // signal EOF to the reader (drops open_ends 2->1)
    __atomic_fetch_add(&s_pr_done, 1u, __ATOMIC_RELEASE);
    g_current->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static void pr_reader_thread(void) {
    uint64_t off = 0;
    for (;;) {
        uint8_t chunk[PR_CHUNK];
        int64_t r = s_pr_r->read(s_pr_r, chunk, PR_CHUNK);
        if (r <= 0) break;   // 0 = EOF, <0 = error
        for (int64_t i = 0; i < r; i++) {
            if (chunk[i] != (uint8_t)((off + (uint64_t)i) & 0xFFu)) {
                __atomic_store_n(&s_pr_fail, 1u, __ATOMIC_RELEASE);
                break;
            }
        }
        off += (uint64_t)r;
    }
    __atomic_store_n(&s_pr_read_total, (uint32_t)off, __ATOMIC_RELEASE);
    s_pr_r->close(s_pr_r);   // last end -> pipe_destroy frees the pipe + both files
    __atomic_fetch_add(&s_pr_done, 1u, __ATOMIC_RELEASE);
    g_current->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

void pipe_race_selftest(void) {
    extern void kprintf(const char*, ...);
    kprintf("[pipe_race] starting: 1 writer + 1 reader, total=%u bytes\n", PR_TOTAL);
    __atomic_store_n(&s_pr_done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pr_fail, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pr_read_total, 0u, __ATOMIC_RELEASE);

    if (pipe_create(&s_pr_r, &s_pr_w) != 0) {
        kprintf_atomic("[pipe_race] FAIL pipe_create\n");
        return;
    }

    task_t* wt = task_create_kthread(pr_writer_thread, pid_alloc());
    task_t* rt = task_create_kthread(pr_reader_thread, pid_alloc());
    if (!wt || !rt) { kprintf_atomic("[pipe_race] FAIL spawn kthreads\n"); return; }
    sched_add(wt);
    sched_add(rt);

    while (__atomic_load_n(&s_pr_done, __ATOMIC_ACQUIRE) != 2)
        cpu_relax();

    uint32_t fail = __atomic_load_n(&s_pr_fail, __ATOMIC_ACQUIRE);
    uint32_t got  = __atomic_load_n(&s_pr_read_total, __ATOMIC_ACQUIRE);
    if (!fail && got == PR_TOTAL) {
        kprintf_atomic("[pipe_race] SELF-TEST PASSED (in-order, no lost/torn bytes, %u/%u)\n",
                got, PR_TOTAL);
    } else {
        kprintf_atomic("[pipe_race] SELF-TEST FAILED (fail=%u got=%u want=%u)\n",
                fail, got, PR_TOTAL);
        for (;;) __asm__ volatile("cli; hlt");
    }
}
