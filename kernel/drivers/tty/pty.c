// ── Pseudo-terminal (PTY) implementation ─────────────────────────────────
//
// Provides openpty() — allocates a master/slave pair.
// The slave is a full tty_t (line discipline, echo, signals).
// The master is a plain fd connected to the slave's I/O.

#include "pty.h"
#include "tty.h"
#include "vfs.h"
#include "errno.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "signal.h"
#include "rcu.h"

// ── PTY_TRACE: targeted serial trace of the pty / makaterm wake chain ───
//
// When enabled, every sleep and wake event in pty_master_write /
// pty_slave_read / pty_master_read / pty_slave_write_buf emits a short
// one-line trace to the serial port (via the locked serial_*_dbg
// helpers).  Used to diagnose the "bash-in-makaterm frozen, no echo"
// hang by showing exactly which link in the chain is silent:
//
//   keyboard → evdev → makadisplay → unix socket → makaterm →
//   pty_master_write → tty_input_char → wake_all(pty->slave.waitq)
//   → pty_slave_read (bash) returns with the input byte
//
// Flip to 0 for silent release builds.  Every trace is one serial
// message, taking the g_serial_lock for the duration of a single
// locked dump — bounded latency, SMP-safe.
#define PTY_TRACE 1

#if PTY_TRACE
#  define PTT(tag) do { \
        serial_puts_dbg("[pty-trace] " tag " pid="); \
        serial_hex_dbg((uint64_t)(g_current ? g_current->pid : 0)); \
    } while (0)
#  define PTT1(tag, lbl, val) do { \
        serial_puts_dbg("[pty-trace] " tag " pid="); \
        serial_hex_dbg((uint64_t)(g_current ? g_current->pid : 0)); \
        serial_puts_dbg("  " lbl "="); \
        serial_hex_dbg((uint64_t)(val)); \
    } while (0)
#else
#  define PTT(tag)              do { } while (0)
#  define PTT1(tag, lbl, val)   do { } while (0)
#endif

// ── Live PTY list ───────────────────────────────────────────────────────
// Singly-linked list of all PTYs that still have at least one fd open.
// Each node is kmalloc'd in pty_alloc and freed when both master and all
// slaves have closed.  No fixed cap.

static pty_t* s_pty_head = NULL;
static uint32_t s_next_pty_index = 0;  // monotonically increasing /dev/pts/N

// Serializes the PTY pair lifetime: the s_pty_head list (insert/unlink/walk),
// s_next_pty_index, and the open-count decision in pty_master_close /
// pty_slave_close.  Without it two concurrent closes (master on one CPU, slave
// on another, fd table shared via CLONE_FILES) both observe "the peer's count
// is 0" and both kfree(pty) -> double-free / UAF; and a concurrent
// open-by-index / ctty walk could deref a node being unlinked.  The close wakes
// run UNDER this lock so a non-freer closer's wake of the peer's embedded waitq
// cannot race the freer's free (the freer is always the SECOND closer to take
// the lock, so the first's wake completes before the second frees).
static spinlock_t s_pty_lock = SPINLOCK_INIT;

pty_t* pty_list_head(void) { return s_pty_head; }

// Unlink pty from s_pty_head.  Caller MUST hold s_pty_lock.  Pairs with the
// locked insert in pty_alloc; once unlinked no walker (open-by-index / ctty
// lookup, also under the lock) can observe it, so the caller can free it after
// releasing the lock.
static void pty_unlink_locked(pty_t* pty) {
    if (s_pty_head == pty) {
        s_pty_head = pty->next;
    } else {
        for (pty_t* p = s_pty_head; p; p = p->next) {
            if (p->next == pty) { p->next = pty->next; break; }
        }
    }
    pty->next = NULL;
}

// Free a pty struct (already unlinked, or never linked -- alloc error path).
// No lock needed: the object is unreachable from s_pty_head.
static void pty_free_struct(pty_t* pty) {
    serial_puts_dbg("[pty] free idx=");
    serial_hex_dbg((uint64_t)(uint32_t)pty->index);
    if (pty->master_buf) kfree(pty->master_buf);
    kfree(pty);
}

// ── Ring buffer helpers (master read buffer) ─────────────────────────────

static inline uint32_t mb_next(uint32_t i) {
    return (i + 1) & (PTY_MASTER_BUF - 1);
}
static inline int mb_empty(pty_t* p) {
    return p->m_head == p->m_tail;
}
static inline int mb_full(pty_t* p) {
    return mb_next(p->m_head) == p->m_tail;
}

// ── Slave write_char callback ────────────────────────────────────────────
// When the slave tty writes output (echo, tty_vfs_write), it ends up here.
// We push into the master's read ring buffer so the terminal emulator can
// read() from the master fd.

static void pty_slave_write_char(tty_t* tty, uint8_t c) {
    // The slave tty is embedded at offset 0 in pty_t.
    pty_t* pty = (pty_t*)tty;

    if (!pty->master_open) return;  // master closed, discard output

    // Push into master ring buffer under master_lock so a concurrent
    // master_read drain (other CPU) cannot race the head/tail update.
    spin_lock(&pty->master_lock);
    if (!mb_full(pty)) {
        pty->master_buf[pty->m_head] = c;
        pty->m_head = mb_next(pty->m_head);
    }
    spin_unlock(&pty->master_lock);
    // Full ring: drop is no longer expected for the normal single-byte
    // echo path.  The batched pty_slave_write_buf applies real flow
    // control instead; this per-byte fallback is only used for line-
    // discipline echo which is at human keyboard rate.
    // Wake every waiter on the master queue — blocking readers
    // (task_we_t) and poll/epoll (epoll_we_t) share the same queue.
    PTT1("slave_write_char.wake", "c", (uint64_t)c);
    wait_queue_wake_all(&pty->master_waitq);
}

// Push one OPOST'd byte into the master ring.  Returns 1 on success,
// 0 if the ring is full (caller must block or give up).  The CALLER must
// hold pty->master_lock (mirrors the tty rd_push "caller holds the lock"
// contract); pty_slave_write_buf takes it around the whole push phase.
static inline int pty_master_push_byte(pty_t* pty, uint8_t c) {
    if (mb_full(pty)) return 0;
    pty->master_buf[pty->m_head] = c;
    pty->m_head = mb_next(pty->m_head);
    return 1;
}

// Batched slave writer: push the whole buffer into the master ring in
// one go, then fire ONE wake_all at the end instead of one per byte.
// ONLCR translation happens here so the ring contains already-OPOST'd
// bytes and the terminal emulator sees proper "\r\n" sequences.
//
// Backpressure: if the ring fills mid-write, the writer blocks on
// pty->slave_drain_waitq until pty_master_read drains some space.
// Linux does the same thing; it's how POSIX ptys achieve lossless
// flow control without allocating an unbounded number of tty_buffer
// chunks.  Wakes the master waitq once per batch chunk so the reader
// can start draining before we've finished.
static void pty_slave_write_buf(tty_t* tty, const uint8_t* buf, uint64_t len) {
    pty_t* pty = (pty_t*)tty;
    if (!pty->master_open || !len) return;

    int opost_onlcr = (tty->termios.c_oflag & OPOST) &&
                      (tty->termios.c_oflag & ONLCR);

    uint64_t i = 0;
    while (i < len) {
        // Phase 1: push as much as fits without blocking, under
        // master_lock so the head update cannot race a master_read drain.
        int pushed_any = 0;
        spin_lock(&pty->master_lock);
        while (i < len) {
            uint8_t c = buf[i];
            if (opost_onlcr && c == '\n') {
                if (!pty_master_push_byte(pty, '\r')) break;
                // Commit the '\r' before trying '\n'.  If '\n' fails we
                // restart this char (still at i).
                if (!pty_master_push_byte(pty, '\n')) {
                    // Uncommit the '\r' we just wrote — walk the head
                    // back.  No reader can have consumed it yet because
                    // we haven't fired wake_all.
                    pty->m_head = (pty->m_head - 1) & (PTY_MASTER_BUF - 1);
                    break;
                }
                i++;
                pushed_any = 1;
                continue;
            }
            if (!pty_master_push_byte(pty, c)) break;
            i++;
            pushed_any = 1;
        }
        spin_unlock(&pty->master_lock);

        // Wake the master reader so it can start draining.
        if (pushed_any) {
            PTT1("slave_write_buf.wake_master", "pushed", i);
            wait_queue_wake_all(&pty->master_waitq);
        }

        // Phase 2: if there's still data left, the ring is full — block
        // until the master drains some space.
        if (i < len) {
            // Master might have gone away during our wait.
            if (!pty->master_open) break;

            WAIT_EVENT(&pty->slave_drain_waitq,
                       !mb_full(pty) || !pty->master_open);
            // Master closed while we slept — bail out.
            if (!pty->master_open) break;
        }
    }
}

// ── Master fd VFS operations ─────────────────────────────────────────────

typedef struct {
    pty_t* pty;
} pty_master_ctx_t;

// Drain up to len bytes from the master ring into out, under master_lock
// (mirrors tty_ldisc_drain).  out is a sys_read buffer whose pages were
// prefaulted by user_buf_prefault before read() was called, so the copy
// under the spinlock cannot fault.  Returns the number of bytes drained.
static uint64_t pty_master_drain(pty_t* pty, uint8_t* out, uint64_t len) {
    uint64_t got = 0;
    spin_lock(&pty->master_lock);
    while (got < len && !mb_empty(pty)) {
        out[got++] = pty->master_buf[pty->m_tail];
        pty->m_tail = mb_next(pty->m_tail);
    }
    spin_unlock(&pty->master_lock);
    return got;
}

// Master read: get slave's output
static int64_t pty_master_read(vfs_file_t* self, void* buf, uint64_t len) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;
    if (!len) return 0;

    // Non-blocking: if no data in buffer, return EAGAIN (or EOF if slave gone).
    if (mb_empty(pty) && (self->flags & 0x800 /*O_NONBLOCK*/)) {
        if (!pty->slave_open_count) return 0; // EOF: slave closed
        return -11; // EAGAIN
    }
    // Block until data available.  Register a task_we_t on the
    // master's wait queue, then re-check — closes the lost-wakeup race.
    PTT1("master_read.enter", "mb_empty", mb_empty(pty));
    if (mb_empty(pty) && !pty->slave_open_count) return 0; // EOF
    WAIT_EVENT_HOOK(&pty->master_waitq,
                    !mb_empty(pty) || !pty->slave_open_count,
                    if (signal_has_actionable(&g_current->sigstate))
                        return -4 /*EINTR*/;);
    if (mb_empty(pty)) return 0;  // woke on EOF

    // Drain under master_lock via the shared helper (mirrors tty_ldisc_drain).
    uint64_t got = pty_master_drain(pty, (uint8_t*)buf, len);
    // Backpressure: if we drained anything, wake slave writers that
    // might be blocked waiting for ring space.
    if (got) wait_queue_wake_all(&pty->slave_drain_waitq);
    PTT1("master_read.return", "got", got);
    return (int64_t)got;
}

// Master write: inject input into slave's line discipline
static int64_t pty_master_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;
    const uint8_t* src = (const uint8_t*)buf;

    PTT1("master_write.enter", "len", len);

    for (uint64_t i = 0; i < len; i++) {
        tty_input_char(&pty->slave, (char)src[i]);
    }
    PTT1("master_write.exit", "len", len);
    return (int64_t)len;
}

static int pty_master_poll(vfs_file_t* self, int events) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;
    int ret = 0;
    if (events & 1 /* POLLIN */)
        ret |= (!mb_empty(pty)) ? 1 : 0;
    if (events & 4 /* POLLOUT */)
        ret |= 4;  // master write is always ready
    return ret;
}

static void pty_slave_close(vfs_file_t* self);

static void pty_master_close(vfs_file_t* self) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;

    // A parked, never-claimed slave handle (ptmx opened, /dev/pts/N
    // never opened) would leak once the master dies — nobody can claim
    // it any more.  Release it through the normal slave close path
    // BEFORE flipping master_open, so pty_slave_close takes the plain
    // "slave closed, master still open" route and the unified free
    // check below stays the single pty_free site.
    if (!pty->slave_claimed && pty->slave_file) {
        vfs_file_t* sf = pty->slave_file;
        pty->slave_file = NULL;
        pty_slave_close(sf);
    }

    // Flip master_open, wake the slave-side waiters (blocking readers/writers,
    // poll/epoll), and decide the free -- all UNDER s_pty_lock so a concurrent
    // pty_slave_close cannot double-free, and so this wake of the slave waitqs
    // cannot race that closer's free of the pty (the freer is whichever closer
    // takes the lock SECOND, so the other's wake has already completed).
    spin_lock(&s_pty_lock);
    pty->master_open = 0;
    wait_queue_wake_all(&pty->slave.waitq);
    wait_queue_wake_all(&pty->slave_drain_waitq);
    int do_free = (pty->slave_open_count == 0);
    if (do_free) pty_unlink_locked(pty);
    spin_unlock(&s_pty_lock);

    kfree(ctx);
    kfree(self);
    if (do_free) pty_free_struct(pty);
}

// ── Slave fd VFS operations ─────────────────────────────────────────────
// These reuse the tty_t infrastructure but with PTY-specific close logic.

typedef struct {
    pty_t* pty;
} pty_slave_ctx_t;

static int64_t pty_slave_read(vfs_file_t* self, void* buf, uint64_t len) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;
    if (!len) return 0;

    // If master is closed, return EIO (like Linux)
    if (!ctx->pty->master_open) return -5; // -EIO

    // POSIX: background process reading from its controlling tty → SIGTTIN.
    if (g_current && tty->fg_pgid &&
        g_current->sid == tty->session &&
        g_current->pgid != tty->fg_pgid) {
        signal_send(g_current, SIGTTIN);
        return -4; // -EINTR
    }

    uint8_t* out = (uint8_t*)buf;
    uint64_t got = 0;

    if (tty->rd_head == tty->rd_tail && !ctx->pty->master_open) return 0; // EOF
    WAIT_EVENT_HOOK(&tty->waitq,
                    tty->rd_head != tty->rd_tail || !ctx->pty->master_open,
                    if (signal_has_actionable(&g_current->sigstate))
                        return -4 /*EINTR*/;);
    if (tty->rd_head == tty->rd_tail) return 0;  // woke on EOF

    // Drain under the slave tty's lock via the shared cooked-byte path -- the
    // same locked drain /dev/tty uses, so the ring is never popped lock-free
    // from two places (dup-shared slave fds can have concurrent readers).
    got = tty_ldisc_drain(tty, out, len);

    PTT1("slave_read.return", "got", got);
    return (int64_t)got;
}

static int64_t pty_slave_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;

    if (!ctx->pty->master_open) return -5; // EIO

    // POSIX: background process writing to its controlling tty → SIGTTOU (if TOSTOP).
    if (g_current && tty->fg_pgid &&
        g_current->sid == tty->session &&
        g_current->pgid != tty->fg_pgid &&
        (tty->termios.c_lflag & TOSTOP)) {
        signal_send(g_current, SIGTTOU);
        return -4; // -EINTR
    }

    // Fast path: batched writer handles ONLCR + ring push + ONE wake_all
    // at the end, instead of one wake_all per byte.  This is the hot
    // path for bash / ps / ls writing to stdout on a pty slave.
    if (tty->write_buf) {
        tty->write_buf(tty, (const uint8_t*)buf, len);
        return (int64_t)len;
    }

    if (!tty->write_char) return (int64_t)len; // discard

    const uint8_t* src = (const uint8_t*)buf;
    for (uint64_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if ((tty->termios.c_oflag & OPOST) && (tty->termios.c_oflag & ONLCR)
            && c == '\n') {
            tty->write_char(tty, '\r');
        }
        tty->write_char(tty, c);
    }
    return (int64_t)len;
}

static int pty_slave_poll(vfs_file_t* self, int events) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;
    int ret = 0;
    if (events & 1 /* POLLIN */)
        ret |= (tty->rd_head != tty->rd_tail) ? 1 : 0;
    if (events & 4 /* POLLOUT */)
        ret |= 4;
    return ret;
}

static void pty_slave_close(vfs_file_t* self) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;

    // Decrement, wake master-side waiters (EOF), and decide the free, all under
    // s_pty_lock (see pty_master_close): serialises the free decision against a
    // concurrent master close and orders this wake before any free.
    spin_lock(&s_pty_lock);
    pty->slave_open_count--;
    wait_queue_wake_all(&pty->master_waitq);
    int do_free = (pty->slave_open_count == 0 && !pty->master_open);
    if (do_free) {
        pty_unlink_locked(pty);
    } else if (pty->slave_file == self) {
        // Master is still open, but the SHARED slave vfs_file_t (self) is freed
        // just below.  Clear the cached pointer + claim flag under the lock so a
        // later /dev/pts/N open sees NULL and does NOT resurrect this freed file
        // via a raw refcount bump -- the reopen-after-slave-close use-after-free.
        pty->slave_file    = NULL;
        pty->slave_claimed = 0;
    }
    spin_unlock(&s_pty_lock);

    kfree(ctx);
    kfree(self);
    if (do_free) pty_free_struct(pty);
}

// ── Master ioctl ─────────────────────────────────────────────────────────
// The PTY master shares the slave's tty_t (winsize, termios, pgid, etc.), so
// TIOCGWINSZ/TIOCSWINSZ/TCGETS/... from the master operate on that same
// struct. Without this handler, sys_ioctl would fall through to a generic
// console-tty fallback that accidentally signals the *console's* fg_pgid —
// which killed makaterm with SIGWINCH on every resize.

// Safe user<->kernel copies (validate + prefault, return 0 / -EFAULT).  The
// ioctl `arg` is a raw user pointer; reading/writing it directly (as the old
// code did) was an arbitrary kernel read/write from any process that can open
// /dev/ptmx or /dev/pts/N.  Route every get/set through these.
extern int copy_to_user(void* dst_u, const void* src, uint64_t len);
extern int copy_from_user(void* dst, const void* src_u, uint64_t len);

// Shared termios/winsize/pgrp ioctls.  Master and slave both act on the SAME
// slave tty, so the get/set logic is identical -- one source of truth here
// instead of two drifting copies.  Returns 0, -EFAULT (bad user `arg`), or
// -EINVAL (request not handled here; the caller handles its unique ioctls).
// TIOCSPGRP authorization: is `pgid` a live process group in session `sid`?
typedef struct { uint32_t sid; int found; } pty_pgsid_t;
static void pty_pgid_sid_visit(task_t* t, void* data) {
    pty_pgsid_t* c = (pty_pgsid_t*)data;
    if (t->sid == c->sid) c->found = 1;
}

static int64_t pty_tty_ioctl_common(tty_t* tty, uint64_t request, uint64_t arg) {
    switch (request) {
        case 0x5401: { // TCGETS
            // Snapshot under tty->lock into a stack local, then copy out -- so a
            // concurrent TCSET* cannot hand userland a half-updated struct, and
            // copy_to_user (which can fault) never touches the live termios.
            termios_t snap;
            tty_get_termios(tty, &snap);
            return copy_to_user((void*)arg, &snap, sizeof(snap)) ? -EFAULT : 0;
        }
        case 0x5402: // TCSETS
        case 0x5403: // TCSETSW
        case 0x5404: { // TCSETSF
            // Land the user copy in a stack local FIRST (copy_from_user can
            // fault, so it must not run under tty->lock), then publish the
            // whole struct atomically-w.r.t-readers via tty_set_termios.  A bad
            // user pointer fails BEFORE any mutation -- no half-set termios.
            termios_t tmp;
            if (copy_from_user(&tmp, (const void*)arg, sizeof(tmp)))
                return -EFAULT;
            tty_set_termios(tty, &tmp);
            if (request == 0x5404) tty_flush_input(tty);
            return 0;
        }
        case 0x540F: { // TIOCGPGRP
            // Snapshot to a local: copy_to_user must not read the LIVE fg_pgid
            // (a concurrent TIOCSPGRP write could be in flight).
            uint32_t pg = __atomic_load_n(&tty->fg_pgid, __ATOMIC_ACQUIRE);
            return copy_to_user((void*)arg, &pg, sizeof(pg)) ? -EFAULT : 0;
        }
        case 0x5410: { // TIOCSPGRP
            // copy_from_user directly INTO the live fg_pgid could tear it (its
            // memcpy(len=4) may lower byte-wise) under a concurrent reader
            // (Ctrl-C / SIGWINCH signal routing).  Land in a local, then publish
            // with one aligned atomic store.  fg_pgid is a single word, so no
            // tty->lock is needed (unlike the multi-word termios snapshot).
            uint32_t pg;
            if (copy_from_user(&pg, (const void*)arg, sizeof(pg))) return -EFAULT;
            // POSIX: the caller must own this controlling terminal (its session)
            // and the new fg pgid must be a process group IN that session.  Without
            // this a process could point the terminal foreground at a FOREIGN
            // session's pgid -- redirecting terminal-generated signals (SIGINT/
            // SIGWINCH) or hijacking the foreground (job-control confused-deputy).
            if (!g_current || g_current->sid != tty->session) return -EPERM;
            if (pg != g_current->pgid) {   // own pgid is trivially in-session
                pty_pgsid_t vc = { g_current->sid, 0 };
                task_idx_pgid_walk(pg, pty_pgid_sid_visit, &vc);
                if (!vc.found) return -EPERM;   // no such pgrp in the caller's session
            }
            __atomic_store_n(&tty->fg_pgid, pg, __ATOMIC_RELEASE);
            return 0;
        }
        case 0x5413: // TIOCGWINSZ
            return copy_to_user((void*)arg, &tty->winsize, sizeof(tty->winsize))
                   ? -EFAULT : 0;
        case 0x5414: // TIOCSWINSZ
            if (copy_from_user(&tty->winsize, (const void*)arg, sizeof(tty->winsize)))
                return -EFAULT;
            // Notify the foreground pgroup so the child shell can reflow.
            if (tty->fg_pgid) signal_send_pgrp(tty->fg_pgid, SIGWINCH);
            return 0;
        case 0x5425: // TCSBRK
        case 0x540B: // TCXONC
        case 0x540C: // TCFLSH
        case 0x540D: // TIOCEXCL
        case 0x540A: // TIOCNXCL
            return 0;  // acknowledged, no-op
        default:
            return -EINVAL;
    }
}

static int64_t pty_master_ioctl(vfs_file_t* self, uint64_t request, uint64_t arg) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    if (request == 0x80045430) { // TIOCGPTN -- master-only: slave index for ptsname()
        uint32_t n = (uint32_t)ctx->pty->index;
        return copy_to_user((void*)arg, &n, sizeof(n)) ? -EFAULT : 0;
    }
    return pty_tty_ioctl_common(&ctx->pty->slave, request, arg);
}

// ── Slave ioctl ──────────────────────────────────────────────────────────

static int64_t pty_slave_ioctl(vfs_file_t* self, uint64_t request, uint64_t arg) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    if (request == 0x540E) { // TIOCSCTTY -- slave-only
        tty_set_ctty(&ctx->pty->slave);
        return 0;
    }
    return pty_tty_ioctl_common(&ctx->pty->slave, request, arg);
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Audit fix: pty ioctls dereferenced the raw user `arg` (arbitrary kernel R/W
// LPE).  Verify every termios/winsize/pgrp/ptn ioctl now rejects a bad user
// pointer (kernel / non-canonical / NULL) with -EFAULT instead of touching it.
void pty_ioctl_selftest(void) {
    extern void kprintf(const char*, ...);
    kprintf("[pty_ioctl_test] pty ioctls must reject bad user pointers\n");
    vfs_file_t* m = pty_open_master();
    if (!m) { kprintf("[pty_ioctl_test] FAIL: pty_open_master\n"); return; }
    static const uint64_t bad[] = {
        0xFFFF800000001000ULL,   // kernel HHDM address (the LPE case)
        0xDEAD000000000000ULL,   // non-canonical
        0ULL,                    // NULL
    };
    static const uint64_t reqs[] = {
        0x5401, 0x5402, 0x540F, 0x5410, 0x5413, 0x5414, 0x80045430,
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (unsigned j = 0; j < sizeof(reqs) / sizeof(reqs[0]); j++) {
            int64_t r = m->ioctl(m, reqs[j], bad[i]);
            if (r != -EFAULT) {
                kprintf("[pty_ioctl_test] FAIL req=0x%lx addr=0x%lx -> %d\n",
                        (unsigned long)reqs[j], (unsigned long)bad[i], (int)r);
                fails++;
            }
        }
    }
    if (m->close) m->close(m);
    kprintf(fails ? "[pty_ioctl_test] SELF-TEST FAILED\n"
                  : "[pty_ioctl_test] SELF-TEST PASSED (ioctls reject bad user pointers)\n");
}
#endif

// ── pty_alloc — create a new PTY pair ────────────────────────────────────

int pty_alloc(vfs_file_t** master_out, vfs_file_t** slave_out) {
    pty_t* pty = (pty_t*)kmalloc(sizeof(pty_t));
    if (!pty) return -12; // ENOMEM

    __builtin_memset(pty, 0, sizeof(pty_t));

    // Out-of-line ring buffer so the pty_t struct stays small and the
    // ring size can be retuned without touching the struct layout.
    pty->master_buf = (uint8_t*)kmalloc(PTY_MASTER_BUF);
    if (!pty->master_buf) { kfree(pty); return -12; }

    pty->master_open = 1;
    pty->slave_open_count = 1;
    // Allocate the /dev/pts index under the lock (advances a global counter).
    // The pty is linked into s_pty_head at the END of alloc, once fully built,
    // so a concurrent open-by-index / ctty walk never observes a half-built node
    // and an alloc-error path just frees it (it was never linked).
    spin_lock(&s_pty_lock);
    pty->index = (int)(s_next_pty_index++);
    spin_unlock(&s_pty_lock);
    int idx = pty->index;

    serial_puts_dbg("[pty] alloc idx=");
    serial_hex_dbg((uint64_t)(uint32_t)idx);

    // Initialize slave tty with sane defaults
    tty_t* tty = &pty->slave;
    tty->termios.c_iflag = ICRNL | IXON;
    tty->termios.c_oflag = OPOST | ONLCR;
    tty->termios.c_cflag = CS8 | CREAD | CLOCAL;
    tty->termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK
                         | ECHOCTL | ECHOKE | IEXTEN;

    uint8_t* cc = tty->termios.c_cc;
    cc[VINTR]    = 3;    // ^C
    cc[VQUIT]    = 28;   /* ^\ */
    cc[VERASE]   = 127;  // DEL
    cc[VKILL]    = 21;   // ^U
    cc[VEOF]     = 4;    // ^D
    cc[VTIME]    = 0;
    cc[VMIN]     = 1;
    cc[VSTART]   = 17;   // ^Q
    cc[VSTOP]    = 19;   // ^S
    cc[VSUSP]    = 26;   // ^Z
    cc[VREPRINT] = 18;   // ^R
    cc[VDISCARD] = 15;   // ^O
    cc[VWERASE]  = 23;   // ^W
    cc[VLNEXT]   = 22;   // ^V

    tty->winsize.ws_row    = 25;
    tty->winsize.ws_col    = 80;
    tty->winsize.ws_xpixel = 640;
    tty->winsize.ws_ypixel = 400;

    tty->write_char = pty_slave_write_char;
    tty->write_buf  = pty_slave_write_buf;

    // Name: "pts/N"
    tty->name[0] = 'p'; tty->name[1] = 't'; tty->name[2] = 's';
    tty->name[3] = '/';
    if (pty->index < 10) {
        tty->name[4] = '0' + pty->index;
        tty->name[5] = '\0';
    } else {
        tty->name[4] = '0' + (pty->index / 10);
        tty->name[5] = '0' + (pty->index % 10);
        tty->name[6] = '\0';
    }

    // Create master vfs_file_t
    pty_master_ctx_t* mctx = kmalloc(sizeof(pty_master_ctx_t));
    if (!mctx) { pty_free_struct(pty); return -12; } // ENOMEM
    mctx->pty = pty;

    vfs_file_t* master = kmalloc(sizeof(vfs_file_t));
    if (!master) { kfree(mctx); pty_free_struct(pty); return -12; }
    __builtin_memset(master, 0, sizeof(*master));

    master->read        = pty_master_read;
    master->write       = pty_master_write;
    master->close       = pty_master_close;
    master->seek        = NULL;
    master->poll        = pty_master_poll;
    master->ioctl       = pty_master_ioctl;
    master->ctx         = mctx;
    master->waitq           = &master->_waitq; wait_queue_init(master->waitq);
    wait_queue_init(&pty->master_waitq);
    wait_queue_init(&pty->slave_drain_waitq);
    master->secondary_waitq = &pty->master_waitq;  // woken by pty_master_push
    master->flags       = 0;
    master->refcount    = 1;
    master->rights      = 0;
    master->path[0]     = '\0';

    pty->master_file = master;  // kept for master close detection (slave EIO path)

    // Create slave vfs_file_t
    pty_slave_ctx_t* sctx = kmalloc(sizeof(pty_slave_ctx_t));
    if (!sctx) { kfree(mctx); kfree(master); pty_free_struct(pty); return -12; }
    sctx->pty = pty;

    vfs_file_t* slave = kmalloc(sizeof(vfs_file_t));
    if (!slave) { kfree(mctx); kfree(sctx); kfree(master); pty_free_struct(pty); return -12; }
    __builtin_memset(slave, 0, sizeof(*slave));

    slave->read     = pty_slave_read;
    slave->write    = pty_slave_write;
    slave->close    = pty_slave_close;
    slave->seek     = NULL;
    slave->poll     = pty_slave_poll;
    slave->ioctl       = pty_slave_ioctl;
    slave->ctx         = sctx;
    slave->waitq           = &slave->_waitq; wait_queue_init(slave->waitq);
    wait_queue_init(&pty->slave.waitq);
    spin_lock_init(&pty->slave.lock);  // ldisc lock for the slave's ring + line buf
    spin_lock_init(&pty->master_lock); // serialises the master-output ring head/tail
    slave->secondary_waitq = &pty->slave.waitq;  // woken by ldisc when data arrives
    slave->flags       = 0;
    slave->refcount    = 1;
    slave->rights      = 0;
    slave->path[0]     = '\0';

    // Publish into the live PTY list now that the pair is fully built (head
    // insert under the lock; pairs with pty_unlink_locked on the close path).
    spin_lock(&s_pty_lock);
    pty->next = s_pty_head;
    s_pty_head = pty;
    spin_unlock(&s_pty_lock);

    *master_out = master;
    *slave_out = slave;
    return 0;
}

// ── /dev/ptmx + /dev/pts/N open paths ───────────────────────────────────
// The posix_openpt model: opening /dev/ptmx allocates a fresh pair and
// returns only the master; the slave handle is parked on the pty until
// the matching /dev/pts/<index> open claims it (foot, sway, every
// terminal emulator does exactly this dance via ptsname()).

vfs_file_t* pty_open_master(void) {
    vfs_file_t* master = NULL;
    vfs_file_t* slave  = NULL;
    if (pty_alloc(&master, &slave) != 0) return NULL;
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)master->ctx;
    ctx->pty->slave_file    = slave;
    ctx->pty->slave_claimed = 0;
    return master;
}

vfs_file_t* pty_open_slave_by_index(int n) {
    // Walk + claim under s_pty_lock so the lookup cannot race a concurrent
    // close that unlinks/frees the node, and the slave_claimed flip / refcount
    // bump is atomic w.r.t. the close-side open-count decision.
    vfs_file_t* result = NULL;
    spin_lock(&s_pty_lock);
    for (pty_t* p = s_pty_head; p; p = p->next) {
        if (p->index != n) continue;
        // slave_file is NULL when the pair is not fully built yet OR when the
        // slave was closed while the master stayed open (pty_slave_close clears
        // it now).  Either way there is no live slave file to hand out -> NULL.
        if (!p->slave_file) break;
        if (!p->slave_claimed) {
            // First open consumes the initial reference taken by
            // pty_alloc (slave_open_count is already 1).
            p->slave_claimed = 1;
            result = p->slave_file;
        } else {
            // Additional opens share the same vfs_file -- dup semantics.
            // vfs_tryget (not a raw refcount bump) fails -> NULL if the file is
            // mid-teardown (refcount already 0), closing the last-close-vs-open
            // race instead of resurrecting a dying object.
            result = vfs_tryget(p->slave_file);
        }
        break;
    }
    spin_unlock(&s_pty_lock);
    return result;
}

// Controlling-tty lookup: return the slave tty whose session == sid, walked
// under s_pty_lock so it cannot deref a node a concurrent close is unlinking.
tty_t* pty_find_ctty_slave(uint32_t sid) {
    tty_t* result = NULL;
    spin_lock(&s_pty_lock);
    for (pty_t* p = s_pty_head; p; p = p->next) {
        if (p->slave.session == sid) { result = &p->slave; break; }
    }
    spin_unlock(&s_pty_lock);
    return result;
}

// Deterministic PTY pair-lifetime selftest (the s_pty_lock fix): a pair is
// freed exactly once -- only after BOTH ends close -- and unlinked from the
// live list.  Single-threaded here (the concurrent master-vs-slave double-free
// is closed by s_pty_lock -> code-proof); runs in a kthread, driving the close
// hooks directly on the vfs_file_t pair.
void pty_lifetime_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    vfs_file_t* m = NULL; vfs_file_t* s = NULL;
    if (pty_alloc(&m, &s) != 0 || !m || !s) {
        kprintf("[pty_life] SELF-TEST FAILED (alloc)\n");
        return;
    }
    pty_t* pty = ((pty_master_ctx_t*)m->ctx)->pty;
    int idx = pty->index;

    // Fresh pair: master open, one slave ref.
    if (pty->master_open != 1 || pty->slave_open_count != 1) {
        fails++;
        kprintf("[pty_life] FAIL fresh m=%d s=%d\n",
                pty->master_open, pty->slave_open_count);
    }

    // Park + claim the slave (the /dev/pts-open path) so it is a real open slave
    // rather than an unclaimed handle the master would release on its own.
    pty->slave_file = s;
    vfs_file_t* claimed = pty_open_slave_by_index(idx);
    if (claimed != s || !pty->slave_claimed) {
        fails++;
        kprintf("[pty_life] FAIL claim\n");
    }

    // Close the master: master_open -> 0, but the slave is still open, so the
    // pty must NOT be freed yet (deref below is safe).
    m->close(m);
    if (pty->master_open != 0 || pty->slave_open_count != 1) {
        fails++;
        kprintf("[pty_life] FAIL after master close m=%d s=%d\n",
                pty->master_open, pty->slave_open_count);
    }

    // Close the slave: slave_open_count -> 0 with the master already closed ->
    // the pty is freed and unlinked.  Do NOT deref pty after this; verify it is
    // gone from the live list by index lookup.
    s->close(s);
    if (pty_open_slave_by_index(idx) != NULL) {
        fails++;
        kprintf("[pty_life] FAIL pty not unlinked after both ends closed\n");
    }

    kprintf(fails ? "[pty_life] SELF-TEST FAILED\n"
                  : "[pty_life] SELF-TEST PASSED (pair freed once, unlinked)\n");
}

// Deterministic check of the reopen-after-slave-close UAF fix: when the slave
// is closed while the master stays OPEN, the shared slave vfs_file_t is freed,
// so pty->slave_file must be CLEARED (not left dangling at the freed file) and a
// subsequent /dev/pts/N reopen must return NULL -- never resurrect the freed
// object with a raw refcount bump (the old __atomic_fetch_add UAF).
void pty_reopen_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    vfs_file_t* m = NULL; vfs_file_t* s = NULL;
    if (pty_alloc(&m, &s) != 0 || !m || !s) {
        kprintf("[pty_reopen] SELF-TEST FAILED (alloc)\n");
        return;
    }
    pty_t* pty = ((pty_master_ctx_t*)m->ctx)->pty;
    int idx = pty->index;

    // Park + claim the slave (the /dev/pts-open path).
    pty->slave_file = s;
    if (pty_open_slave_by_index(idx) != s || !pty->slave_claimed) fails++;

    // Close the slave with the master STILL open: the pty survives, but the
    // shared slave vfs_file_t is freed.  Do NOT deref `s` after this.
    s->close(s);
    if (pty->slave_file != NULL)     fails++;   // dangling pointer cleared (the fix)
    if (pty->slave_claimed != 0)     fails++;
    if (pty->slave_open_count != 0)  fails++;

    // Reopen must return NULL, never a resurrected freed file.
    if (pty_open_slave_by_index(idx) != NULL) fails++;

    // Tear down: with the slave gone, closing the master frees the pty.
    m->close(m);

    kprintf(fails ? "[pty_reopen] SELF-TEST FAILED\n"
                  : "[pty_reopen] SELF-TEST PASSED (slave_file cleared on close, no reopen UAF)\n");
}

// Deterministic master-ring selftest (the master_lock fix): drive the producer
// (slave write_char + write_buf) and the consumer (pty_master_drain), all of
// which now take master_lock, and verify the ring round-trips with no self-
// deadlock.  The concurrent producer-vs-consumer race is not deterministically
// reproducible -> this proves the locked single-threaded discipline; code-proof
// covers the cross-CPU case (master_lock makes the head/tail mutations atomic,
// mirroring tty->lock over the slave input ring).
void pty_master_ring_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    vfs_file_t* m = NULL; vfs_file_t* s = NULL;
    if (pty_alloc(&m, &s) != 0 || !m || !s) {
        kprintf("[pty_mring] SELF-TEST FAILED (alloc)\n");
        return;
    }
    pty_t* pty = ((pty_master_ctx_t*)m->ctx)->pty;

    // Producer A: per-byte path (locked push).
    pty_slave_write_char(&pty->slave, 'h');
    pty_slave_write_char(&pty->slave, 'i');
    // Producer B: batched path (locked Phase-1 push); 5 bytes fit, no block.
    pty_slave_write_buf(&pty->slave, (const uint8_t*)"there", 5);

    // Consumer: drain via the locked helper -- expect "hithere".
    uint8_t out[8];
    __builtin_memset(out, 0, sizeof(out));
    uint64_t got = pty_master_drain(pty, out, sizeof(out));
    const char* exp = "hithere";
    int match = (got == 7);
    for (uint64_t k = 0; match && k < 7; k++)
        if (out[k] != (uint8_t)exp[k]) match = 0;
    if (!match) {
        fails++;
        kprintf("[pty_mring] FAIL roundtrip got=%lu\n", (unsigned long)got);
    }
    // Ring is drained empty; a second drain returns 0 (head == tail).
    if (pty_master_drain(pty, out, sizeof(out)) != 0) {
        fails++;
        kprintf("[pty_mring] FAIL ring not empty after drain\n");
    }

    // Tear down: close both ends (master then slave) so the pair is freed and
    // unlinked, exactly like pty_lifetime_selftest.
    m->close(m);
    s->close(s);

    kprintf(fails ? "[pty_mring] SELF-TEST FAILED\n"
                  : "[pty_mring] SELF-TEST PASSED (locked master-ring push/drain round-trip)\n");
}
