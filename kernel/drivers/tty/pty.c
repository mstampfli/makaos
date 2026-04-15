// ── Pseudo-terminal (PTY) implementation ─────────────────────────────────
//
// Provides openpty() — allocates a master/slave pair.
// The slave is a full tty_t (line discipline, echo, signals).
// The master is a plain fd connected to the slave's I/O.

#include "pty.h"
#include "tty.h"
#include "vfs.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "signal.h"

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

pty_t* pty_list_head(void) { return s_pty_head; }

// Remove pty from s_pty_head and free it.  Called when both master_open==0
// and slave_open_count==0.
static void pty_free_locked(pty_t* pty) {
    if (s_pty_head == pty) {
        s_pty_head = pty->next;
    } else {
        for (pty_t* p = s_pty_head; p; p = p->next) {
            if (p->next == pty) { p->next = pty->next; break; }
        }
    }
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

    // Push into master ring buffer
    if (!mb_full(pty)) {
        pty->master_buf[pty->m_head] = c;
        pty->m_head = mb_next(pty->m_head);
    }
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
// 0 if the ring is full (caller must block or give up).
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
        // Phase 1: push as much as fits without blocking.
        int pushed_any = 0;
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

            task_we_t node;
            task_we_init(&node, g_current);
            task_we_add(&pty->slave_drain_waitq, &node);
            // Re-check under the registration to close the lost-wakeup
            // window.  If the ring drained between phase 1's last push
            // and the task_we_add above, pty_master_read already fired
            // a wake, which would have found no waiters — so without the
            // recheck we'd sleep forever.
            if (mb_full(pty) && pty->master_open)
                sched_sleep();
            task_we_remove(&pty->slave_drain_waitq, &node);

            // Master closed while we slept — bail out.
            if (!pty->master_open) break;
        }
    }
}

// ── Master fd VFS operations ─────────────────────────────────────────────

typedef struct {
    pty_t* pty;
} pty_master_ctx_t;

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
    for (;;) {
        if (!mb_empty(pty)) break;
        if (!pty->slave_open_count) return 0;  // EOF: slave closed

        task_we_t node;
        task_we_init(&node, g_current);
        task_we_add(&pty->master_waitq, &node);
        if (mb_empty(pty) && pty->slave_open_count) {
            PTT("master_read.sleep");
            sched_sleep();
            PTT("master_read.wake");
        }
        task_we_remove(&pty->master_waitq, &node);

        if (signal_has_actionable(&g_current->sigstate))
            return -4; // EINTR
    }

    uint8_t* out = (uint8_t*)buf;
    uint64_t got = 0;
    while (got < len && !mb_empty(pty)) {
        out[got++] = pty->master_buf[pty->m_tail];
        pty->m_tail = mb_next(pty->m_tail);
    }
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

static void pty_master_close(vfs_file_t* self) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;

    pty->master_open = 0;

    // Wake any slave reader (they'll get EOF / EIO).  Blocking readers
    // sleep on slave.waitq, poll/epoll waiters too — one wake_all
    // fires both.
    wait_queue_wake_all(&pty->slave.waitq);
    // Also wake any slave writer blocked on a full ring — they'll
    // notice master_open == 0 and bail out.
    wait_queue_wake_all(&pty->slave_drain_waitq);

    kfree(ctx);
    kfree(self);

    // If both sides are now closed, free the pty struct.
    if (pty->slave_open_count == 0)
        pty_free_locked(pty);
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

    uint32_t vmin = (tty->termios.c_lflag & ICANON) ? 1
                  : tty->termios.c_cc[VMIN];
    if (vmin == 0) vmin = 1;

    for (;;) {
        if (tty->rd_head != tty->rd_tail) {
            PTT1("slave_read.wake.data", "head", tty->rd_head);
            break;
        }
        if (!ctx->pty->master_open) {
            PTT("slave_read.eof");
            return 0; // EOF
        }

        task_we_t node;
        task_we_init(&node, g_current);
        task_we_add(&tty->waitq, &node);
        PTT1("slave_read.registered", "waitq", (uint64_t)&tty->waitq);
        if (tty->rd_head == tty->rd_tail && ctx->pty->master_open) {
            PTT("slave_read.sleep");
            sched_sleep();
            PTT("slave_read.wake");
        }
        task_we_remove(&tty->waitq, &node);

        if (signal_has_actionable(&g_current->sigstate)) {
            PTT("slave_read.eintr");
            return -4; // EINTR
        }
    }

    while (got < len) {
        if (tty->rd_head == tty->rd_tail) break;
        uint8_t c = tty->read_buf[tty->rd_head];
        tty->rd_head = (tty->rd_head + 1) & (TTY_READ_BUF_SIZE - 1);
        out[got++] = c;
        if ((tty->termios.c_lflag & ICANON) && c == '\n') break;
        if (!(tty->termios.c_lflag & ICANON) && got >= vmin) break;
    }

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

    pty->slave_open_count--;

    // Wake every master-side waiter so they observe EOF.
    wait_queue_wake_all(&pty->master_waitq);

    kfree(ctx);
    kfree(self);

    // If both sides are now closed, free the pty struct.
    if (pty->slave_open_count == 0 && !pty->master_open)
        pty_free_locked(pty);
}

// ── Master ioctl ─────────────────────────────────────────────────────────
// The PTY master shares the slave's tty_t (winsize, termios, pgid, etc.), so
// TIOCGWINSZ/TIOCSWINSZ/TCGETS/... from the master operate on that same
// struct. Without this handler, sys_ioctl would fall through to a generic
// console-tty fallback that accidentally signals the *console's* fg_pgid —
// which killed makaterm with SIGWINCH on every resize.

static int64_t pty_master_ioctl(vfs_file_t* self, uint64_t request, uint64_t arg) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;

    switch (request) {
        case 0x5401: { // TCGETS
            termios_t* out = (termios_t*)arg;
            *out = tty->termios;
            return 0;
        }
        case 0x5402: // TCSETS
        case 0x5403: // TCSETSW
        case 0x5404: { // TCSETSF
            const termios_t* in = (const termios_t*)arg;
            tty->termios = *in;
            if (request == 0x5404) tty_flush_input(tty);
            return 0;
        }
        case 0x540F: { // TIOCGPGRP
            uint32_t* out = (uint32_t*)arg;
            *out = tty->fg_pgid;
            return 0;
        }
        case 0x5410: { // TIOCSPGRP
            const uint32_t* in = (const uint32_t*)arg;
            tty->fg_pgid = *in;
            return 0;
        }
        case 0x5413: { // TIOCGWINSZ
            winsize_t* out = (winsize_t*)arg;
            *out = tty->winsize;
            return 0;
        }
        case 0x5414: { // TIOCSWINSZ
            const winsize_t* in = (const winsize_t*)arg;
            tty->winsize = *in;
            // Notify the foreground pgroup on the slave side so the child
            // shell can reflow. Never signal the master's own pgroup.
            if (tty->fg_pgid)
                signal_send_pgrp(tty->fg_pgid, SIGWINCH);
            return 0;
        }
        case 0x5425: // TCSBRK
        case 0x540B: // TCXONC
        case 0x540C: // TCFLSH
        case 0x540D: // TIOCEXCL
        case 0x540A: // TIOCNXCL
            return 0;
        default:
            return -22; // EINVAL
    }
}

// ── Slave ioctl ──────────────────────────────────────────────────────────

static int64_t pty_slave_ioctl(vfs_file_t* self, uint64_t request, uint64_t arg) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;

    switch (request) {
        case 0x5401: { // TCGETS
            termios_t* out = (termios_t*)arg;
            *out = tty->termios;
            return 0;
        }
        case 0x5402: // TCSETS
        case 0x5403: // TCSETSW
        case 0x5404: { // TCSETSF
            const termios_t* in = (const termios_t*)arg;
            tty->termios = *in;
            if (request == 0x5404) tty_flush_input(tty);
            return 0;
        }
        case 0x540F: { // TIOCGPGRP
            uint32_t* out = (uint32_t*)arg;
            *out = tty->fg_pgid;
            return 0;
        }
        case 0x5410: { // TIOCSPGRP
            const uint32_t* in = (const uint32_t*)arg;
            tty->fg_pgid = *in;
            return 0;
        }
        case 0x5413: { // TIOCGWINSZ
            winsize_t* out = (winsize_t*)arg;
            *out = tty->winsize;
            return 0;
        }
        case 0x5414: { // TIOCSWINSZ
            const winsize_t* in = (const winsize_t*)arg;
            tty->winsize = *in;
            // TODO: send SIGWINCH to fg_pgid
            return 0;
        }
        case 0x540E: // TIOCSCTTY
            tty_set_ctty(tty);
            return 0;
        case 0x5425: // TCSBRK
        case 0x540B: // TCXONC
        case 0x540C: // TCFLSH
        case 0x540D: // TIOCEXCL
        case 0x540A: // TIOCNXCL
            return 0;  // acknowledged, no-op
        default:
            return -22; // EINVAL
    }
}

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
    pty->index = (int)(s_next_pty_index++);
    int idx = pty->index;

    // Link into the live PTY list (head insert).
    pty->next = s_pty_head;
    s_pty_head = pty;

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
    if (!mctx) { pty_free_locked(pty); return -12; } // ENOMEM
    mctx->pty = pty;

    vfs_file_t* master = kmalloc(sizeof(vfs_file_t));
    if (!master) { kfree(mctx); pty_free_locked(pty); return -12; }

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
    if (!sctx) { kfree(mctx); kfree(master); pty_free_locked(pty); return -12; }
    sctx->pty = pty;

    vfs_file_t* slave = kmalloc(sizeof(vfs_file_t));
    if (!slave) { kfree(mctx); kfree(sctx); kfree(master); pty_free_locked(pty); return -12; }

    slave->read     = pty_slave_read;
    slave->write    = pty_slave_write;
    slave->close    = pty_slave_close;
    slave->seek     = NULL;
    slave->poll     = pty_slave_poll;
    slave->ioctl       = pty_slave_ioctl;
    slave->ctx         = sctx;
    slave->waitq           = &slave->_waitq; wait_queue_init(slave->waitq);
    wait_queue_init(&pty->slave.waitq);
    slave->secondary_waitq = &pty->slave.waitq;  // woken by ldisc when data arrives
    slave->flags       = 0;
    slave->refcount    = 1;
    slave->rights      = 0;
    slave->path[0]     = '\0';

    *master_out = master;
    *slave_out = slave;
    return 0;
}
