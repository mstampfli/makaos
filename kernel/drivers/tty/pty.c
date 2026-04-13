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

// ── Global PTY table ────────────────────────────────────────────────────

pty_t g_ptys[PTY_MAX];

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
    // Wake master reader if sleeping in blocking read().
    if (pty->m_reader) {
        sched_wake(pty->m_reader);
        pty->m_reader = NULL;
    }
    // Wake all poll/epoll waiters on the master fd.
    wait_queue_wake_all(&pty->master_waitq);
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
    // Block until data available
    while (mb_empty(pty)) {
        if (!pty->slave_open_count) return 0;  // EOF: slave closed
        pty->m_reader = g_current;
        sched_sleep();
        if (g_current->sigstate.head != g_current->sigstate.tail)
            return -4; // EINTR
    }

    uint8_t* out = (uint8_t*)buf;
    uint64_t got = 0;
    while (got < len && !mb_empty(pty)) {
        out[got++] = pty->master_buf[pty->m_tail];
        pty->m_tail = mb_next(pty->m_tail);
    }
    return (int64_t)got;
}

// Master write: inject input into slave's line discipline
static int64_t pty_master_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pty_master_ctx_t* ctx = (pty_master_ctx_t*)self->ctx;
    pty_t* pty = ctx->pty;
    const uint8_t* src = (const uint8_t*)buf;

    for (uint64_t i = 0; i < len; i++) {
        tty_input_char(&pty->slave, (char)src[i]);
    }
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

    // Wake any slave reader (they'll get EOF / EIO)
    if (pty->slave.reader) {
        sched_wake(pty->slave.reader);
        pty->slave.reader = NULL;
    }

    // If slave is also closed, free the PTY slot
    if (pty->slave_open_count == 0)
        pty->allocated = 0;

    kfree(ctx);
    kfree(self);
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

    uint8_t* out = (uint8_t*)buf;
    uint64_t got = 0;

    uint32_t vmin = (tty->termios.c_lflag & ICANON) ? 1
                  : tty->termios.c_cc[VMIN];
    if (vmin == 0) vmin = 1;

    while (tty->rd_head == tty->rd_tail) {
        if (!ctx->pty->master_open) return 0; // EOF
        tty->reader = g_current;
        sched_sleep();
        if (g_current->sigstate.head != g_current->sigstate.tail)
            return -4; // EINTR
    }

    while (got < len) {
        if (tty->rd_head == tty->rd_tail) break;
        uint8_t c = tty->read_buf[tty->rd_head];
        tty->rd_head = (tty->rd_head + 1) & (TTY_READ_BUF_SIZE - 1);
        out[got++] = c;
        if ((tty->termios.c_lflag & ICANON) && c == '\n') break;
        if (!(tty->termios.c_lflag & ICANON) && got >= vmin) break;
    }

    return (int64_t)got;
}

static int64_t pty_slave_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pty_slave_ctx_t* ctx = (pty_slave_ctx_t*)self->ctx;
    tty_t* tty = &ctx->pty->slave;

    if (!ctx->pty->master_open) return -5; // EIO

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

    // If both sides closed, free the slot
    if (pty->slave_open_count == 0 && !pty->master_open)
        pty->allocated = 0;

    // Wake master reader (EOF)
    if (pty->m_reader) {
        sched_wake(pty->m_reader);
        pty->m_reader = NULL;
    }

    kfree(ctx);
    kfree(self);
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
    // Find a free slot
    pty_t* pty = NULL;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].allocated) {
            pty = &g_ptys[i];
            pty->index = i;
            break;
        }
    }
    if (!pty) return -23; // ENFILE

    int idx = pty->index;

    // Zero the struct
    for (uint64_t i = 0; i < sizeof(pty_t); i++)
        ((uint8_t*)pty)[i] = 0;

    pty->allocated = 1;
    pty->master_open = 1;
    pty->slave_open_count = 1;
    pty->index = idx;

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
    if (!mctx) { pty->allocated = 0; return -12; } // ENOMEM
    mctx->pty = pty;

    vfs_file_t* master = kmalloc(sizeof(vfs_file_t));
    if (!master) { kfree(mctx); pty->allocated = 0; return -12; }

    master->read        = pty_master_read;
    master->write       = pty_master_write;
    master->close       = pty_master_close;
    master->seek        = NULL;
    master->poll        = pty_master_poll;
    master->ioctl       = pty_master_ioctl;
    master->ctx         = mctx;
    master->waitq           = &master->_waitq; wait_queue_init(master->waitq);
    wait_queue_init(&pty->master_waitq);
    master->secondary_waitq = &pty->master_waitq;  // woken by pty_master_push
    master->flags       = 0;
    master->refcount    = 1;
    master->rights      = 0;
    master->path[0]     = '\0';

    pty->master_file = master;  // kept for master close detection (slave EIO path)

    // Create slave vfs_file_t
    pty_slave_ctx_t* sctx = kmalloc(sizeof(pty_slave_ctx_t));
    if (!sctx) { kfree(mctx); kfree(master); pty->allocated = 0; return -12; }
    sctx->pty = pty;

    vfs_file_t* slave = kmalloc(sizeof(vfs_file_t));
    if (!slave) { kfree(mctx); kfree(sctx); kfree(master); pty->allocated = 0; return -12; }

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
