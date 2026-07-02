// ── TTY subsystem — N_TTY line discipline + VFS glue ─────────────────────
//
// One tty_t (tty0) backed by the PS/2 keyboard and framebuffer.
// Future ttys (serial, pty) slot in identically.

#include "tty.h"
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
#include "pty.h"
#include "input_core.h"
#include "errno.h"   // EPERM (tty_set_fg_pgrp)
#include "vfs.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "rcu.h"
#include "signal.h"
#include "fb.h"
#include "preempt.h"
#include "smp.h"

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

// Number of free (writable) slots in the ring.  Usable capacity is size-1
// (one slot reserved so full != empty).  Pure -> unit-tested
// (tty_rb_free_selftest); used by ldisc_flush_line for an all-or-nothing
// commit so a partial flush can never drop a cooked line's terminating '\n'.
static inline uint32_t rb_free(uint32_t head, uint32_t tail, uint32_t size) {
    return (head - tail - 1u) & (size - 1u);
}

// ── Line-discipline lock ──────────────────────────────────────────────────
// preempt_disable + plain spinlock.  The producer is a kernel thread (console)
// or a syscall (pty), never an IRQ handler, so no irqsave is needed -- see the
// lock comment in tty.h.  Mirrors the fb.c console-lock pattern: spin_lock is a
// bare test-and-set and the kernel is preemptible, so preempt_disable is
// required to stop this CPU switching away while holding it.
static inline void tty_lock(tty_t* tty)   { preempt_disable(); spin_lock(&tty->lock); }
static inline void tty_unlock(tty_t* tty) { spin_unlock(&tty->lock); preempt_enable(); }

// PRIMITIVE: termios publish / snapshot (consistent multi-word copy) ────────
// TCSET* overwrites the WHOLE termios, and tty->termios = *src is a multi-word
// copy.  A lock-free reader (tty_input_char's signal/canon decisions,
// tty_ldisc_drain's canon/VMIN read) racing that write on another CPU could
// observe a half-updated struct -- a new c_lflag paired with an old c_cc[]
// byte -- and swallow a ^C, fire a spurious SIGINT/SIGTSTP/SIGQUIT at the
// foreground pgrp, or drain a line in the wrong mode.  Publishing under the
// lock alone is NOT enough: the reader must take the same lock or it can still
// catch the assignment mid-flight.  So all termios writers publish through
// tty_set_termios and all readers that need a consistent view snapshot through
// tty_get_termios -- the copy is always made under tty->lock and is never
// observed mid-update.  copy_from_user (which can fault) must run into a stack
// local FIRST, OUTSIDE the lock, since tty->lock is a preempt-disabled leaf
// spinlock.  One source of truth for both ends, so they cannot drift.
void tty_set_termios(tty_t* tty, const termios_t* src) {
    tty_lock(tty);
    tty->termios = *src;
    tty_unlock(tty);
}
void tty_get_termios(tty_t* tty, termios_t* dst) {
    tty_lock(tty);
    *dst = tty->termios;
    tty_unlock(tty);
}

// ── Set foreground process group (SINGLE SOURCE OF TRUTH) ──────────────────
// TIOCSPGRP / tcsetpgrp on BOTH ptys and the console route through here, so the
// authorization can never drift (the console + tcsetpgrp paths used to set
// fg_pgid with NO check -> any process, even a foreign session, could point the
// terminal foreground at an arbitrary pgrp and hijack Ctrl-C/Z/\\ -- the vuln
// beae63c fixed for ptys only).  POSIX: the caller must own this controlling
// terminal (its session) AND the new pgid must be a process group IN that
// session.  Publishes with an ordered atomic store (pairs the readers' acquire
// load).  Returns 0, or -EPERM if unauthorized.
typedef struct { uint32_t sid; int found; } tty_pgsid_t;
static void tty_pgid_sid_visit(task_t* t, void* data) {
    tty_pgsid_t* c = (tty_pgsid_t*)data;
    if (t->sid == c->sid) c->found = 1;
}
int tty_set_fg_pgrp(tty_t* tty, uint32_t pg) {
    if (!g_current || g_current->sid != tty->session) return -EPERM;
    if (pg != g_current->pgid) {   // own pgid is trivially in-session
        tty_pgsid_t vc = { g_current->sid, 0 };
        task_idx_pgid_walk(pg, tty_pgid_sid_visit, &vc);
        if (!vc.found) return -EPERM;   // no such pgrp in the caller's session
    }
    __atomic_store_n(&tty->fg_pgid, pg, __ATOMIC_RELEASE);
    return 0;
}

// POSIX job control: caller is a background process for this controlling tty.
// One source of truth for the SIGTTIN (bg read) and SIGTTOU (bg write, +TOSTOP)
// gates in tty.c and pty.c.
int tty_is_background(const tty_t* tty) {
    return g_current && tty->fg_pgid &&
           g_current->sid == tty->session &&
           g_current->pgid != tty->fg_pgid;
}

// ── Ring-buffer push/pop ──────────────────────────────────────────────────
// PURE ring ops: the CALLER must hold tty->lock.  The ring is mutated from
// several CPUs (producer, consumer(s), and tty_flush_input), so every access
// goes through the lock -- there is no lock-free fast path here.
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
// write_char is fb I/O (takes the fb lock) -- NEVER call this under tty->lock.
static void tty_echo(tty_t* tty, uint8_t c) {
    if (tty->write_char) tty->write_char(tty, c);
}

// Echo the visual erase of one character: backspace, space, backspace.
static void tty_echo_erase(tty_t* tty) {
    if (tty->termios.c_lflag & ECHO) {
        tty_echo(tty, '\b');
        tty_echo(tty, ' ');
        tty_echo(tty, '\b');
    }
}

// ── Canonical line-buffer mutations ────────────────────────────────────────
// Each takes tty->lock around the state change only; the caller does any echo
// AFTER the call (echo is fb I/O and must stay outside the lock).

// Append one byte to the canonical line if there is room; returns 1 if stored.
static int ldisc_line_append(tty_t* tty, uint8_t c) {
    int stored = 0;
    tty_lock(tty);
    if (tty->line_len < TTY_LINE_BUF_SIZE - 1) {
        tty->line_buf[tty->line_len++] = c;
        stored = 1;
    }
    tty_unlock(tty);
    return stored;
}

// Remove one byte from the canonical line; returns 1 if a byte was erased.
static int ldisc_line_erase(tty_t* tty) {
    int erased = 0;
    tty_lock(tty);
    if (tty->line_len > 0) { tty->line_len--; erased = 1; }
    tty_unlock(tty);
    return erased;
}

// Zero the canonical line; returns the prior length (for the ^U echo count).
static uint32_t ldisc_line_kill(tty_t* tty) {
    tty_lock(tty);
    uint32_t n = tty->line_len;
    tty->line_len = 0;
    tty_unlock(tty);
    return n;
}

// ── Flush canonical line buffer to read_buf, then wake readers ──────────
static void ldisc_flush_line(tty_t* tty) {
    // All-or-nothing commit, under the lock: only commit the line if the WHOLE
    // line fits in the ring's current free space.  A per-byte push that ran out
    // of room mid-line would drop the trailing bytes -- including the
    // terminating '\n' -- so the reader would get a '\n'-less partial line that
    // merges with the next one (or hangs a canonical read waiting for a '\n'
    // that was discarded).  If it doesn't fit (reader far behind / input queue
    // full), drop the WHOLE line: POSIX-acceptable input loss, not framing
    // corruption.
    tty_lock(tty);
    if (rb_free(tty->rd_head, tty->rd_tail, TTY_READ_BUF_SIZE) >= tty->line_len) {
        for (uint32_t i = 0; i < tty->line_len; i++)
            rd_push(tty, tty->line_buf[i]);
    }
    tty->line_len = 0;
    tty_unlock(tty);
    // Wake every waiter on the tty's queue -- blocking readers register
    // task_we_t nodes, poll/epoll registers epoll_we_t nodes.  A single
    // wake_all drains them all.  Done OUTSIDE the lock (it touches the
    // scheduler's rq_lock, which must never nest under tty->lock).
    wait_queue_wake_all(&tty->waitq);
}

// ── N_TTY: process one input character ────────────────────────────────────
void tty_input_char(tty_t* tty, char c) {
    if (!c) return;

    // Take ONE consistent snapshot of termios up front (under tty->lock, via
    // tty_get_termios) so a concurrent TCSET* on another CPU cannot make us
    // mix a new c_lflag with an old c_cc[] byte mid-character.  Every termios
    // read below uses this snapshot, never the live struct.
    termios_t tio;
    tty_get_termios(tty, &tio);
    uint32_t lflag = tio.c_lflag;
    const uint8_t* cc = tio.c_cc;

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

        // Erase character (backspace / DEL).  Mutate under the lock, echo after.
        if ((uint8_t)c == cc[VERASE] || c == '\b') {
            if (ldisc_line_erase(tty)) tty_echo_erase(tty);
            return;
        }
        // Kill line (^U): erase entire current line in one locked step, then
        // echo the visual erase for each removed char plus a trailing newline.
        if ((uint8_t)c == cc[VKILL]) {
            uint32_t n = ldisc_line_kill(tty);
            if (lflag & ECHO) {
                for (uint32_t i = 0; i < n; i++) {
                    tty_echo(tty, '\b'); tty_echo(tty, ' '); tty_echo(tty, '\b');
                }
                tty_echo(tty, '\n');
            }
            return;
        }
        // EOF (^D): flush line (even if empty — empty flush signals EOF).
        if ((uint8_t)c == cc[VEOF]) {
            ldisc_flush_line(tty);
            return;
        }
        // Newline: accumulate then flush.  Echo the '\n' (outside the lock)
        // only if it was actually stored.
        if (c == '\n' || (uint8_t)c == cc[VEOL] || (uint8_t)c == cc[VEOL2]) {
            if (ldisc_line_append(tty, '\n') && (lflag & ECHO))
                tty_echo(tty, '\n');
            ldisc_flush_line(tty);
            return;
        }
        // Carriage return → newline (ICRNL).
        if (c == '\r' && (tio.c_iflag & ICRNL)) {
            tty_input_char(tty, '\n');
            return;
        }
        // Regular character: accumulate under the lock, echo after if stored.
        if (ldisc_line_append(tty, (uint8_t)c) && (lflag & ECHO))
            tty_echo(tty, (uint8_t)c);
        return;
    }

    // ── Raw mode ─────────────────────────────────────────────────────────
    // CR→NL conversion even in raw mode if ICRNL is set.
    if (c == '\r' && (tio.c_iflag & ICRNL)) c = '\n';

    if (lflag & ECHO) tty_echo(tty, (uint8_t)c);     // echo before push (fb I/O, no lock)
    tty_lock(tty);
    rd_push(tty, (uint8_t)c);
    tty_unlock(tty);
    wait_queue_wake_all(&tty->waitq);                // wake outside the lock
}

// ── Flush input buffers ───────────────────────────────────────────────────
// Resets the ring + canonical line under the lock so it cannot tear a
// concurrent producer's rd_push / line accumulation or a consumer's rd_pop.
// Callers (the ^C/^Z/^\ path in tty_input_char, and ioctl TCFLSH/TCSETSF) must
// NOT already hold tty->lock -- the lock is non-recursive.
void tty_flush_input(tty_t* tty) {
    tty_lock(tty);
    tty->rd_head = tty->rd_tail = 0;
    tty->line_len = 0;
    tty_unlock(tty);
}

// ── Shared cooked-byte drain ──────────────────────────────────────────────
// Single locked drain path used by BOTH /dev/tty reads and pty-slave reads,
// so the ring is never popped lock-free from two places.  Pops up to `len`
// bytes, honouring canonical mode (stop after '\n', one line per read) and
// raw VMIN.  Never blocks -- the caller does the wait first -- and must be
// called WITHOUT holding tty->lock.  Returns the number of bytes copied.
uint64_t tty_ldisc_drain(tty_t* tty, uint8_t* out, uint64_t len) {
    uint64_t got = 0;
    tty_lock(tty);
    // Read the mode/VMIN under the SAME lock that guards the ring, so a
    // concurrent TCSET* cannot tear ICANON against c_cc[VMIN] here (reuses the
    // acquisition we already need for the drain loop -- no extra lock cost).
    int canon = (tty->termios.c_lflag & ICANON) != 0;
    uint32_t vmin = canon ? 1 : tty->termios.c_cc[VMIN];
    if (vmin == 0) vmin = 1;  // always read at least 1
    while (got < len) {
        uint8_t ch;
        if (!rd_pop(tty, &ch)) break;
        out[got++] = ch;
        if (canon && ch == '\n') break;     // one line per canonical read
        if (!canon && got >= vmin) break;    // raw VMIN satisfied
    }
    tty_unlock(tty);
    return got;
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
    if (tty_is_background(tty)) {
        signal_send(g_current, SIGTTIN);
        return -4; // -EINTR
    }

    // Block until at least one byte is available, using the canonical
    // Phase 9-6 wait-queue pattern:
    //
    //   Phase 1 — unregistered fast check.  If data already exists we
    //             skip the queue dance entirely.
    //   Phase 2 — register task_we_t on tty->waitq BEFORE the real
    //             check, so any future wake_all finds us.
    //   Phase 3 — re-check under the registration.  Catches the race
    //             where input landed between Phase 1 and Phase 2 and
    //             fired wake_all before we were on the queue.
    //   Phase 4 — sched_sleep.  If the waker ran between Phase 2 and
    //             here, sched_sleep's wake_pending interlock bails
    //             out without actually parking.
    //   task_we_remove on every exit path — stack entry must leave
    //             the queue before its frame is dropped.
    //
    // Signals with SIG_DFL-ignore disposition (SIGCHLD, SIGWINCH) must
    // NOT interrupt the read — they're silently discarded on the syscall
    // return path, and returning EINTR here causes an infinite loop
    // since the signal stays queued until signal_deliver_pending runs.
    WAIT_EVENT_HOOK(&tty->waitq,
                    !rb_empty(tty->rd_head, tty->rd_tail),
                    if (signal_has_actionable(&g_current->sigstate))
                        return -4 /*EINTR*/;);

    // Drain under the per-tty lock via the shared cooked-byte path.
    return (int64_t)tty_ldisc_drain(tty, (uint8_t*)buf, len);
}

static int64_t tty_vfs_write(vfs_file_t* self, const void* buf, uint64_t len) {
    tty_ctx_t* ctx = (tty_ctx_t*)self->ctx;
    tty_t* tty = ctx->tty;
    const uint8_t* src = (const uint8_t*)buf;

    // POSIX: background process writing to its controlling tty → SIGTTOU (if TOSTOP).
    if (tty_is_background(tty) && (tty->termios.c_lflag & TOSTOP)) {
        signal_send(g_current, SIGTTOU);
        return -4; // -EINTR
    }

    // Fast path: if the backend provides a batched write_buf, use it.
    // This avoids per-byte preempt_disable toggles (fb) and per-byte
    // wake_all storms (pty slave) for the common case of bash/ps
    // emitting a whole line at once.
    if (tty->write_buf) {
        tty->write_buf(tty, src, len);
        return (int64_t)len;
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

    vfs_file_t* f = vfs_alloc_file();   // zeroed, waitq wired, refcount=1
    if (!f) { kfree(ctx); return NULL; }
    f->read  = tty_vfs_read;
    f->write = tty_vfs_write;
    f->close = tty_vfs_close;
    f->poll  = tty_vfs_poll;
    f->ctx   = ctx;
    f->secondary_waitq = &tty->waitq;  // non-default: woken by ldisc when data arrives
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
    // PTY slaves -- locked lookup in pty.c (race-free vs a concurrent close that
    // unlinks/frees a node; an open-coded pty_list_head() walk could deref it).
    return pty_find_ctty_slave(g_current->sid);
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

// Batched version: emits the whole user buffer with a single
// preempt_disable section, honouring OPOST|ONLCR '\n' → "\r\n" as it
// goes.  Called from tty_vfs_write when it sees tty->write_buf != NULL.
static void console_write_buf(tty_t* tty, const uint8_t* buf, uint64_t len) {
    extern void fb_term_write(const char* buf, uint64_t len);
#ifdef MAKAOS_CONSOLE_SERIAL
    // Opt-in (CONSOLE_SERIAL=1 build): mirror userland console output to the
    // serial port so headless boots are diagnosable and userland program/test
    // output is capturable (e.g. POSIX conformance runs).  Default-off — it
    // would flood the serial under a running desktop.  '\n' -> "\r\n" so a
    // serial terminal renders cleanly.
    {
        extern uint64_t serial_lock_irqsave(void);
        extern void     serial_unlock_irqrestore(uint64_t);
        extern void     serial_raw_putc(char);
        uint64_t _sf = serial_lock_irqsave();
        for (uint64_t _i = 0; _i < len; _i++) {
            if (buf[_i] == '\n') serial_raw_putc('\r');
            serial_raw_putc((char)buf[_i]);
        }
        serial_unlock_irqrestore(_sf);
    }
#endif
    int opost_onlcr = (tty->termios.c_oflag & OPOST) &&
                      (tty->termios.c_oflag & ONLCR);
    if (!opost_onlcr) {
        fb_term_write((const char*)buf, len);
        return;
    }
    // ONLCR path: split into runs separated by '\n' so we can emit each
    // run as one batched memcpy-equivalent loop, with a single "\r\n"
    // at the boundary.  Avoids copying into a scratch buffer.
    uint64_t run_start = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > run_start)
                fb_term_write((const char*)(buf + run_start), i - run_start);
            fb_term_write("\r\n", 2);
            run_start = i + 1;
        }
    }
    if (run_start < len)
        fb_term_write((const char*)(buf + run_start), len - run_start);
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
    spin_lock_init(&tty->lock);
    wait_queue_init(&tty->waitq);
    tty->write_char = console_write_char;
    tty->write_buf  = console_write_buf;

    __builtin_memset(tty->name, 0, sizeof(tty->name));
    tty->name[0] = 't'; tty->name[1] = 't'; tty->name[2] = 'y';
    tty->name[3] = '0'; tty->name[4] = '\0';

    // Register with input_core so keyboard events flow in via tty_on_kbd_event.
    // INPUT_HANDLER_CONSOLE tells input_emit to skip us while a GUI grabber
    // holds the keyboard — otherwise a Ctrl+C typed in a compositor window
    // also fires VINTR on tty0, which sends SIGINT to tty0's fg_pgid (the
    // compositor's child process tree).  Linux: identical to KD_GRAPHICS.
    tty->input_handler.name  = tty->name;
    tty->input_handler.flags = INPUT_HANDLER_CONSOLE;
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

// ── rb_free selftest ──────────────────────────────────────────────────────
// Deterministic check of the ring free-slot arithmetic that makes
// ldisc_flush_line all-or-nothing (so a cooked line's terminating '\n' is
// never dropped by a partial flush), plus the invariant that the read ring is
// strictly larger than a maximum cooked line.
void tty_rb_free_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    // size=8 ring (usable capacity 7): head, tail -> expected free slots.
    struct { uint32_t head, tail, want; } c[] = {
        { 0, 0, 7 },   // empty -> all usable slots free
        { 0, 1, 6 },   // 1 byte used
        { 0, 7, 0 },   // full (rb_next(7)=0=head)
        { 3, 3, 7 },   // empty at an offset
        { 5, 2, 2 },   // head=5,tail=2: 5 used -> 2 free
        { 2, 5, 4 },   // head=2,tail=5: 3 used -> 4 free
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        uint32_t got = rb_free(c[i].head, c[i].tail, 8u);
        if (got != c[i].want) {
            kprintf_atomic("[tty_rbfree] FAIL head=%u tail=%u got=%u want=%u\n",
                    c[i].head, c[i].tail, got, c[i].want);
            fails++;
        }
    }
    // Framing invariant: an empty read ring must hold a whole maximum cooked
    // line (TTY_LINE_BUF_SIZE-1 bytes incl. '\n') -- the size bump guarantees it.
    uint32_t empty_free = rb_free(0u, 0u, TTY_READ_BUF_SIZE);
    if (empty_free < (TTY_LINE_BUF_SIZE - 1u)) {
        kprintf_atomic("[tty_rbfree] FAIL ring too small: free=%u < maxline=%u\n",
                empty_free, (unsigned)(TTY_LINE_BUF_SIZE - 1u));
        fails++;
    }
    kprintf_atomic(fails ? "[tty_rbfree] SELF-TEST FAILED\n"
                  : "[tty_rbfree] SELF-TEST PASSED (ring free-slots + line fits)\n");
}

// ── ldisc lock selftest ────────────────────────────────────────────────────
// Drives the EXACT locked line-discipline path -- tty_input_char (producer),
// tty_ldisc_drain (consumer), tty_flush_input -- and asserts byte-correct
// output.  Single-threaded, so it does not reproduce the cross-CPU race (that
// is a code-proof), but it proves the lock path is BALANCED and cannot HANG: a
// missing unlock or a recursive re-acquire of the non-recursive leaf lock would
// spin forever on this CPU (preempt disabled) -> the boot would freeze here and
// never print PASSED -- and that the locked push/drain/flush move bytes
// correctly.  Uses kmalloc (no permanent BSS in the shipping kernel).
void tty_ldisc_selftest(void) {
    extern void kprintf(const char*, ...);
    tty_t* t = (tty_t*)kmalloc(sizeof(tty_t));
    if (!t) { kprintf_atomic("[tty_ldisc] SELF-TEST SKIP (no mem)\n"); return; }
    __builtin_memset(t, 0, sizeof(*t));
    spin_lock_init(&t->lock);
    wait_queue_init(&t->waitq);
    t->write_char = NULL;   // no echo -> no fb dependency
    t->fg_pgid    = 0;      // no job-control signals
    int fails = 0;
    uint8_t buf[16];
    uint64_t n;

    // (A) Canonical: a completed line flushes intact; drain stops at '\n'.
    t->termios.c_iflag = 0;
    t->termios.c_lflag = ICANON;
    tty_input_char(t, 'h'); tty_input_char(t, 'e');
    tty_input_char(t, 'y'); tty_input_char(t, '\n');
    n = tty_ldisc_drain(t, buf, sizeof(buf));
    if (n != 4 || buf[0] != 'h' || buf[1] != 'e' || buf[2] != 'y' || buf[3] != '\n') {
        kprintf_atomic("[tty_ldisc] FAIL canon n=%lu\n", (unsigned long)n); fails++;
    }

    // (B) tty_flush_input discards a pending (un-flushed) canonical line.
    tty_input_char(t, 'x'); tty_input_char(t, 'y');   // accumulating, not flushed
    tty_flush_input(t);                                // discard line + ring
    tty_input_char(t, 'z'); tty_input_char(t, '\n');   // fresh line "z\n"
    n = tty_ldisc_drain(t, buf, sizeof(buf));
    if (n != 2 || buf[0] != 'z' || buf[1] != '\n') {
        kprintf_atomic("[tty_ldisc] FAIL flush-pending n=%lu\n", (unsigned long)n); fails++;
    }

    // (C) Raw push lands in the ring; tty_flush_input then clears the ring.
    t->termios.c_lflag    = 0;
    t->termios.c_cc[VMIN] = 1;
    tty_input_char(t, 'Q');
    n = tty_ldisc_drain(t, buf, sizeof(buf));          // raw VMIN=1 -> 1 byte
    if (n != 1 || buf[0] != 'Q') {
        kprintf_atomic("[tty_ldisc] FAIL raw n=%lu\n", (unsigned long)n); fails++;
    }
    tty_input_char(t, 'R'); tty_input_char(t, 'S');    // two bytes queued
    tty_flush_input(t);                                // clear the ring
    n = tty_ldisc_drain(t, buf, sizeof(buf));          // ring empty -> 0
    if (n != 0) {
        kprintf_atomic("[tty_ldisc] FAIL flush-ring n=%lu\n", (unsigned long)n); fails++;
    }

    kfree(t);
    kprintf_atomic(fails ? "[tty_ldisc] SELF-TEST FAILED\n"
                  : "[tty_ldisc] SELF-TEST PASSED (locked push/drain/flush, no self-deadlock)\n");
}

// ── termios publish/snapshot selftest ──────────────────────────────────────
// Proves the tty_set_termios / tty_get_termios primitive that closes the
// TCSET*-vs-line-discipline data race: a published termios is snapshotted back
// byte-for-byte (the whole struct is copied, not a partial view), republishing
// fully overwrites it, and the line discipline (tty_input_char/tty_ldisc_drain)
// honours the mode that was PUBLISHED through the locked path -- so the readers
// really do observe set_termios's value, not a stale or torn one.  Single
// threaded (the cross-CPU tear is a code-proof: both ends now take tty->lock),
// but a missing/recursive unlock in the primitive would hang here under
// preempt-disable and never print PASSED.  No __builtin_memcmp (no kernel
// symbol) -- bytes are compared by hand.
void tty_termios_snapshot_selftest(void) {
    extern void kprintf(const char*, ...);
    tty_t* t = (tty_t*)kmalloc(sizeof(tty_t));
    if (!t) { kprintf_atomic("[tty_termios] SELF-TEST SKIP (no mem)\n"); return; }
    __builtin_memset(t, 0, sizeof(*t));
    spin_lock_init(&t->lock);
    wait_queue_init(&t->waitq);
    t->write_char = NULL;   // no echo -> no fb dependency
    t->fg_pgid    = 0;      // no job-control signals
    int fails = 0;

    // (A) Publish a fully-populated termios; snapshot must match every byte.
    termios_t a;
    __builtin_memset(&a, 0, sizeof(a));
    a.c_iflag = ICRNL;
    a.c_lflag = ICANON | ECHO | ISIG;
    a.c_line  = 7;
    for (unsigned i = 0; i < NCCS; i++) a.c_cc[i] = (uint8_t)(0x10 + i);
    tty_set_termios(t, &a);
    termios_t snap;
    tty_get_termios(t, &snap);
    const uint8_t* pa = (const uint8_t*)&a;
    const uint8_t* ps = (const uint8_t*)&snap;
    for (unsigned i = 0; i < sizeof(termios_t); i++) {
        if (pa[i] != ps[i]) {
            kprintf_atomic("[tty_termios] FAIL snapshot byte %u: %u != %u\n",
                    i, (unsigned)pa[i], (unsigned)ps[i]);
            fails++; break;
        }
    }

    // (B) Republish a DIFFERENT termios; the snapshot must fully overwrite --
    // no field from (A) survives.  Catches a partial publish.
    termios_t b;
    __builtin_memset(&b, 0, sizeof(b));
    b.c_lflag = 0;                 // raw
    b.c_cc[VMIN] = 3;
    tty_set_termios(t, &b);
    tty_get_termios(t, &snap);
    if (snap.c_lflag != 0 || snap.c_iflag != 0 || snap.c_line != 0 ||
        snap.c_cc[VMIN] != 3) {
        kprintf_atomic("[tty_termios] FAIL republish stale lflag=%u iflag=%u\n",
                (unsigned)snap.c_lflag, (unsigned)snap.c_iflag);
        fails++;
    }

    // (C) The line discipline must obey the PUBLISHED mode.  Canonical first:
    // a completed line drains at '\n'.
    termios_t canon;
    __builtin_memset(&canon, 0, sizeof(canon));
    canon.c_lflag = ICANON;
    tty_set_termios(t, &canon);
    uint8_t buf[16];
    tty_input_char(t, 'h'); tty_input_char(t, 'i'); tty_input_char(t, '\n');
    uint64_t n = tty_ldisc_drain(t, buf, sizeof(buf));
    if (n != 3 || buf[0] != 'h' || buf[1] != 'i' || buf[2] != '\n') {
        kprintf_atomic("[tty_termios] FAIL canon-after-publish n=%lu\n", (unsigned long)n);
        fails++;
    }

    // (D) Publish raw (VMIN=1): a single char is now immediately drainable,
    // proving the reader picked up the newly published mode.
    termios_t raw;
    __builtin_memset(&raw, 0, sizeof(raw));
    raw.c_lflag = 0;
    raw.c_cc[VMIN] = 1;
    tty_set_termios(t, &raw);
    tty_input_char(t, 'Z');
    n = tty_ldisc_drain(t, buf, sizeof(buf));
    if (n != 1 || buf[0] != 'Z') {
        kprintf_atomic("[tty_termios] FAIL raw-after-publish n=%lu\n", (unsigned long)n);
        fails++;
    }

    kfree(t);
    kprintf_atomic(fails ? "[tty_termios] SELF-TEST FAILED\n"
                  : "[tty_termios] SELF-TEST PASSED (publish/snapshot consistent, ldisc obeys published mode)\n");
}
