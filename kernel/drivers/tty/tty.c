// ── TTY subsystem — N_TTY line discipline + VFS glue ─────────────────────
//
// One tty_t (tty0) backed by the PS/2 keyboard and framebuffer.
// Future ttys (serial, pty) slot in identically.

#include "tty.h"
#include "pty.h"
#include "input_core.h"
#include "vfs.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "signal.h"
#include "fb.h"

// ── Physical console TTY ─────────────────────────────────────────────────
tty_t g_tty0;

// ── Ring-buffer helpers ───────────────────────────────────────────────────

static inline uint32_t rb_next(uint32_t i, uint32_t size) {
    return (i + 1) & (size - 1);
}

static inline int rb_empty(uint32_t head, uint32_t tail) {
    return head == tail;
}

static inline int rb_full(uint32_t head, uint32_t tail, uint32_t size) {
    return rb_next(tail, size) == head;
}

static void rd_push(tty_t* tty, uint8_t c) {
    if (rb_full(tty->rd_head, tty->rd_tail, TTY_READ_BUF_SIZE)) return; // drop
    tty->read_buf[tty->rd_tail] = c;
    tty->rd_tail = rb_next(tty->rd_tail, TTY_READ_BUF_SIZE);
}

static int rd_pop(tty_t* tty, uint8_t* out) {
    if (rb_empty(tty->rd_head, tty->rd_tail)) return 0;
    *out = tty->read_buf[tty->rd_head];
    tty->rd_head = rb_next(tty->rd_head, TTY_READ_BUF_SIZE);
    return 1;
}

// ── Echo a character to the terminal output ───────────────────────────────
static void tty_echo(tty_t* tty, uint8_t c) {
    if (tty->write_char) tty->write_char(tty, c);
}

// ── Erase one character from the canonical line buffer ───────────────────
static void ldisc_erase_char(tty_t* tty) {
    if (tty->line_len == 0) return;
    tty->line_len--;
    if (tty->termios.c_lflag & ECHO) {
        tty_echo(tty, '\b');
        tty_echo(tty, ' ');
        tty_echo(tty, '\b');
    }
}

// ── Flush canonical line buffer to read_buf, then wake readers ──────────
static void ldisc_flush_line(tty_t* tty) {
    for (uint32_t i = 0; i < tty->line_len; i++)
        rd_push(tty, tty->line_buf[i]);
    tty->line_len = 0;
    // Wake every waiter on the tty's queue — blocking readers register
    // task_we_t nodes, poll/epoll registers epoll_we_t nodes.  A single
    // wake_all drains them all.
    wait_queue_wake_all(&tty->waitq);
}

// ── N_TTY: process one input character ────────────────────────────────────
void tty_input_char(tty_t* tty, char c) {
    if (!c) return;

    uint32_t lflag = tty->termios.c_lflag;
    const uint8_t* cc = tty->termios.c_cc;

    // ── Signal characters (ISIG) ─────────────────────────────────────────
    if (lflag & ISIG) {
        if ((uint8_t)c == cc[VINTR]) {   // ^C → SIGINT to fg pgrp
            if (tty->fg_pgid) {
                extern void signal_send_pgrp(uint32_t pgid, int sig);
                signal_send_pgrp(tty->fg_pgid, SIGINT);
            }
            if (!(lflag & NOFLSH)) tty_flush_input(tty);
            return;
        }
        if ((uint8_t)c == cc[VSUSP]) {   // ^Z → SIGTSTP to fg pgrp
            if (tty->fg_pgid) {
                extern void signal_send_pgrp(uint32_t pgid, int sig);
                signal_send_pgrp(tty->fg_pgid, SIGTSTP);
            }
            if (!(lflag & NOFLSH)) tty_flush_input(tty);
            return;
        }
        if ((uint8_t)c == cc[VQUIT]) {   // ^\ → SIGQUIT to fg pgrp
            if (tty->fg_pgid) {
                extern void signal_send_pgrp(uint32_t pgid, int sig);
                signal_send_pgrp(tty->fg_pgid, SIGQUIT);
            }
            if (!(lflag & NOFLSH)) tty_flush_input(tty);
            return;
        }
    }

    // ── Canonical mode (ICANON) ──────────────────────────────────────────
    if (lflag & ICANON) {
        // ── ANSI escape sequence filter ───────────────────────────────────
        // Strip CSI sequences (ESC [ ... letter) and OSC/SS3 sequences so
        // arrow keys and other function keys don't appear as raw text.
        if ((uint8_t)c == 0x1B) { tty->esc_state = 1; return; }
        if (tty->esc_state == 1) {
            tty->esc_state = ((uint8_t)c == '[' || (uint8_t)c == 'O') ? 2 : 0;
            return;
        }
        if (tty->esc_state == 2) {
            // Consume until final byte (0x40–0x7E).
            if ((uint8_t)c >= 0x40 && (uint8_t)c <= 0x7E) tty->esc_state = 0;
            return;
        }

        // Erase character (backspace / DEL).
        if ((uint8_t)c == cc[VERASE] || c == '\b') {
            ldisc_erase_char(tty);
            return;
        }
        // Kill line (^U): erase entire current line.
        if ((uint8_t)c == cc[VKILL]) {
            while (tty->line_len > 0) ldisc_erase_char(tty);
            if (lflag & ECHO) tty_echo(tty, '\n');
            return;
        }
        // EOF (^D): flush line (even if empty — empty flush signals EOF).
        if ((uint8_t)c == cc[VEOF]) {
            ldisc_flush_line(tty);
            return;
        }
        // Newline: accumulate then flush.
        if (c == '\n' || (uint8_t)c == cc[VEOL] || (uint8_t)c == cc[VEOL2]) {
            if (tty->line_len < TTY_LINE_BUF_SIZE - 1) {
                if (lflag & ECHO) tty_echo(tty, '\n');
                tty->line_buf[tty->line_len++] = '\n';
            }
            ldisc_flush_line(tty);
            return;
        }
        // Carriage return → newline (ICRNL).
        if (c == '\r' && (tty->termios.c_iflag & ICRNL)) {
            tty_input_char(tty, '\n');
            return;
        }
        // Regular character: accumulate.
        if (tty->line_len < TTY_LINE_BUF_SIZE - 1) {
            if (lflag & ECHO) tty_echo(tty, (uint8_t)c);
            tty->line_buf[tty->line_len++] = (uint8_t)c;
        }
        return;
    }

    // ── Raw mode ─────────────────────────────────────────────────────────
    // CR→NL conversion even in raw mode if ICRNL is set.
    if (c == '\r' && (tty->termios.c_iflag & ICRNL)) c = '\n';

    if (lflag & ECHO) tty_echo(tty, (uint8_t)c);
    rd_push(tty, (uint8_t)c);
    wait_queue_wake_all(&tty->waitq);
}

// ── Flush input buffers ───────────────────────────────────────────────────
void tty_flush_input(tty_t* tty) {
    tty->rd_head = tty->rd_tail = 0;
    tty->line_len = 0;
}

// ── VFS operations for /dev/ttyN ─────────────────────────────────────────

typedef struct {
    tty_t* tty;
} tty_ctx_t;

static int64_t tty_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    tty_ctx_t* ctx = (tty_ctx_t*)self->ctx;
    tty_t* tty = ctx->tty;
    if (!len) return 0;

    // POSIX: background process reading from its controlling tty → SIGTTIN.
    // Only applies when this tty is the process's controlling terminal.
    if (g_current && tty->fg_pgid &&
        g_current->sid == tty->session &&
        g_current->pgid != tty->fg_pgid) {
        signal_send(g_current, SIGTTIN);
        return -4; // -EINTR
    }

    uint8_t* out = (uint8_t*)buf;
    uint64_t got = 0;

    // In raw mode with VMIN/VTIME: we implement VMIN here.
    uint32_t vmin = (tty->termios.c_lflag & ICANON) ? 1
                  : tty->termios.c_cc[VMIN];
    if (vmin == 0) vmin = 1; // always read at least 1

    // Block until at least vmin bytes are available.  Each iteration
    // registers a fresh task_we_t on the tty's wait queue, then
    // re-checks the buffer.  Registration BEFORE the check closes the
    // lost-wakeup race: if input arrives between our check and the
    // sched_sleep, wait_queue_wake_all will find our entry and wake us.
    //
    // Signals with SIG_DFL-ignore disposition (SIGCHLD, SIGWINCH) must
    // NOT interrupt the read — they're silently discarded on the syscall
    // return path, and returning EINTR here causes an infinite loop
    // since the signal stays queued until signal_deliver_pending runs.
    for (;;) {
        task_we_t node;
        task_we_init(&node, g_current);
        task_we_add(&tty->waitq, &node);

        if (!rb_empty(tty->rd_head, tty->rd_tail)) {
            task_we_remove(&tty->waitq, &node);
            break;
        }

        sched_sleep();
        task_we_remove(&tty->waitq, &node);

        if (signal_has_actionable(&g_current->sigstate))
            return -4; // -EINTR
    }

    // Drain up to len bytes.
    while (got < len) {
        uint8_t c;
        if (!rd_pop(tty, &c)) break;
        out[got++] = c;
        // In canonical mode, stop at newline (one line per read).
        if ((tty->termios.c_lflag & ICANON) && c == '\n') break;
        // In raw mode, stop at VMIN.
        if (!(tty->termios.c_lflag & ICANON) && got >= vmin) break;
    }
    return (int64_t)got;
}

static int64_t tty_vfs_write(vfs_file_t* self, const void* buf, uint64_t len) {
    tty_ctx_t* ctx = (tty_ctx_t*)self->ctx;
    tty_t* tty = ctx->tty;
    const uint8_t* src = (const uint8_t*)buf;

    // POSIX: background process writing to its controlling tty → SIGTTOU (if TOSTOP).
    if (g_current && tty->fg_pgid &&
        g_current->sid == tty->session &&
        g_current->pgid != tty->fg_pgid &&
        (tty->termios.c_lflag & TOSTOP)) {
        signal_send(g_current, SIGTTOU);
        return -4; // -EINTR
    }

    if (!tty->write_char) return (int64_t)len;  // no output backend (e.g. compositor owns fb)
    for (uint64_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        // OPOST + ONLCR: translate \n → \r\n on output.
        if ((tty->termios.c_oflag & OPOST) && (tty->termios.c_oflag & ONLCR)
            && c == '\n') {
            tty->write_char(tty, '\r');
        }
        tty->write_char(tty, c);
    }
    return (int64_t)len;
}

static void tty_vfs_close(vfs_file_t* self) {
    kfree(self->ctx);
    kfree(self);
}

static int tty_vfs_poll(vfs_file_t* self, int events) {
    tty_ctx_t* ctx = (tty_ctx_t*)self->ctx;
    tty_t* tty = ctx->tty;
    if (events & 1 /*POLLIN*/)
        return !rb_empty(tty->rd_head, tty->rd_tail);
    return 1;
}

// ── tty_open ─────────────────────────────────────────────────────────────
vfs_file_t* tty_open(int idx) {
    if (idx != 0) return NULL;
    tty_t* tty = &g_tty0;

    tty_ctx_t* ctx = kmalloc(sizeof(tty_ctx_t));
    if (!ctx) return NULL;
    ctx->tty = tty;

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(ctx); return NULL; }

    f->read     = tty_vfs_read;
    f->write    = tty_vfs_write;
    f->close    = tty_vfs_close;
    f->seek     = NULL;   // ttys are not seekable
    f->poll           = tty_vfs_poll;
    f->ioctl          = NULL;  // tty0 ioctl handled by sys_ioctl fallback
    f->ctx            = ctx;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = &tty->waitq;  // woken by ldisc when data arrives
    f->flags          = 0;
    f->refcount    = 1;
    f->rights   = 0;   // device fd: no rights enforcement (checked as open by kernel)
    f->path[0]  = '\0';

    return f;
}

// ── tty_get_ctty / tty_set_ctty ──────────────────────────────────────────
//
// Returns the controlling tty of the current task.  Each tty stores its
// session id (tty->session), set by TIOCSCTTY.  We find the tty whose
// session matches g_current->sid.  Two candidates exist: the physical
// console (g_tty0) and any open PTY slaves.  No iteration over tasks is
// needed — the tty-side lookup is O(1) for g_tty0 and O(live PTYs) for
// the PTY list, which is tiny.
tty_t* tty_get_ctty(void) {
    if (!g_current) return NULL;
    // Physical console TTY.
    if (g_tty0.session == g_current->sid) return &g_tty0;
    // PTY slaves — walk the live PTY list (maintained in pty.c).
    for (pty_t* p = pty_list_head(); p; p = p->next) {
        if (p->slave.session == g_current->sid) return &p->slave;
    }
    return NULL;
}

void tty_set_ctty(tty_t* tty) {
    if (!g_current || !tty) return;
    tty->session  = g_current->sid;
    tty->fg_pgid  = g_current->pgid;
}

// ── Console write_char: write a byte to the framebuffer terminal ──────────
static void console_write_char(tty_t* tty, uint8_t c) {
    (void)tty;
    extern void fb_term_putc(char c);
    fb_term_putc((char)c);
}

// forward declaration — defined below tty_init
static void tty_on_kbd_event(const kbd_event_t* ev, void* data);

// ── tty_init ─────────────────────────────────────────────────────────────
void tty_init(void) {
    tty_t* tty = &g_tty0;

    // Default termios: canonical, echo, signals enabled.
    tty->termios.c_iflag = ICRNL | IXON;
    tty->termios.c_oflag = OPOST | ONLCR;
    tty->termios.c_cflag = CS8 | CREAD | CLOCAL;
    tty->termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK
                         | ECHOCTL | ECHOKE | IEXTEN;
    tty->termios.c_line  = 0;

    uint8_t* cc = tty->termios.c_cc;
    cc[VINTR]    = 3;    // ^C
    cc[VQUIT]    = 28;   /* ^\ */
    cc[VERASE]   = 127;  // DEL
    cc[VKILL]    = 21;   // ^U
    cc[VEOF]     = 4;    // ^D
    cc[VTIME]    = 0;
    cc[VMIN]     = 1;
    cc[VSWTC]    = 0;
    cc[VSTART]   = 17;   // ^Q
    cc[VSTOP]    = 19;   // ^S
    cc[VSUSP]    = 26;   // ^Z
    cc[VEOL]     = 0;
    cc[VREPRINT] = 18;   // ^R
    cc[VDISCARD] = 15;   // ^O
    cc[VWERASE]  = 23;   // ^W
    cc[VLNEXT]   = 22;   // ^V
    cc[VEOL2]    = 0;

    tty->winsize.ws_row    = 50;   // matches our fb terminal rows
    tty->winsize.ws_col    = 160;  // matches our fb terminal cols
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;

    tty->fg_pgid   = 1;
    tty->session   = 1;
    tty->rd_head   = 0;
    tty->rd_tail   = 0;
    tty->line_len  = 0;
    wait_queue_init(&tty->waitq);
    tty->write_char = console_write_char;

    __builtin_memset(tty->name, 0, sizeof(tty->name));
    tty->name[0] = 't'; tty->name[1] = 't'; tty->name[2] = 'y';
    tty->name[3] = '0'; tty->name[4] = '\0';

    // Register with input_core so keyboard events flow in via tty_on_kbd_event.
    tty->input_handler.name  = tty->name;
    tty->input_handler.event = tty_on_kbd_event;
    tty->input_handler.data  = tty;
    tty->input_handler.next  = NULL;
    input_register_handler(&tty->input_handler);
}

// ── input_core handler: receive kbd_event_t, route to tty_input_char ─────
// Extended keys inject ANSI escape sequences; printable keys go directly.
// Key-release events (pressed=0) are ignored — TTY only cares about presses.

static void tty_on_kbd_event(const kbd_event_t* ev, void* data) {
    tty_t* tty = (tty_t*)data;

    // Ignore key releases.
    if (!ev->pressed) return;

    // Extended navigation keys → ANSI CSI sequences (raw mode gets them as-is;
    // canonical mode strips the ESC sequence via esc_state filter).
    if (ev->keycode) {
        const char* seq = NULL;
        switch (ev->keycode) {
            case KEY_UP:       seq = "\x1b[A";  break;
            case KEY_DOWN:     seq = "\x1b[B";  break;
            case KEY_RIGHT:    seq = "\x1b[C";  break;
            case KEY_LEFT:     seq = "\x1b[D";  break;
            case KEY_HOME:     seq = "\x1b[H";  break;
            case KEY_END:      seq = "\x1b[F";  break;
            case KEY_PAGEUP:   seq = "\x1b[5~"; break;
            case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
            case KEY_DELETE:   seq = "\x1b[3~"; break;
            case KEY_F1:       seq = "\x1b[11~";break;
            case KEY_F2:       seq = "\x1b[12~";break;
            case KEY_F3:       seq = "\x1b[13~";break;
            case KEY_F4:       seq = "\x1b[14~";break;
            case KEY_F5:       seq = "\x1b[15~";break;
            case KEY_F6:       seq = "\x1b[17~";break;
            case KEY_F7:       seq = "\x1b[18~";break;
            case KEY_F8:       seq = "\x1b[19~";break;
            case KEY_F9:       seq = "\x1b[20~";break;
            case KEY_F10:      seq = "\x1b[21~";break;
            case KEY_F11:      seq = "\x1b[23~";break;
            case KEY_F12:      seq = "\x1b[24~";break;
            default: break;
        }
        if (seq) {
            for (int i = 0; seq[i]; i++) tty_input_char(tty, seq[i]);
            return;
        }
    }

    // Printable / control character.
    if (ev->ascii) tty_input_char(tty, (char)ev->ascii);
}
