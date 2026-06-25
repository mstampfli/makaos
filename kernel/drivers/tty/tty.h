#pragma once
#include "common.h"
#include "syscall.h"    // termios_t, winsize_t, ICANON, ECHO, ISIG, …
#include "input_core.h" // input_handler_t, kbd_event_t
#include "wait.h"       // wait_queue_t

// Debug trace — flip to 0 for silent release.  See pty.c for the
// PTT / PTT1 macros that use this.  Placed in the shared header so
// pty.c and tty.c agree.
#define PTY_TRACE 0

// ── TTY subsystem ────────────────────────────────────────────────────────
//
// Architecture:
//   keyboard IRQ → tty_input_char(tty, c)
//                       ↓
//                  N_TTY line discipline
//                  ├── canonical: accumulate in line_buf, deliver on \n/EOF
//                  │   handle BS (erase), ^U (kill), ^C/^Z (signal), ^D (EOF)
//                  └── raw: push every char directly to read_buf
//                       ↓
//                  read_buf ring buffer
//                       ↓
//                  vfs_file_t read() → blocks via sched_sleep until data ready
//
// One tty_t per physical terminal (tty0 = framebuffer+PS/2 keyboard).
// Future: serial ttys, ptys — all use the same struct and ops.

// ── Buffer sizes ──────────────────────────────────────────────────────────
// READ must be strictly larger than LINE so a full cooked line (up to
// TTY_LINE_BUF_SIZE-1 bytes incl. its '\n') always fits in the ring with room
// to spare even when a reader is behind -- otherwise ldisc_flush_line's
// per-byte push could drop the line's tail (incl. the terminating '\n') and
// corrupt canonical line framing.  Keep both powers of 2 (rb_* mask with -1).
#define TTY_READ_BUF_SIZE  8192   // power of 2; cooked output to readers (> LINE)
#define TTY_LINE_BUF_SIZE  4096   // canonical line accumulation buffer

// ── tty_t ─────────────────────────────────────────────────────────────────
typedef struct tty_t {
    // ── Termios (line discipline config) ─────────────────────────────────
    termios_t termios;
    winsize_t winsize;

    // ── Foreground process group (job control) ───────────────────────────
    uint32_t fg_pgid;

    // ── Session this tty is the controlling terminal of ──────────────────
    uint32_t session;

    // ── Read buffer: data ready for userland read() ───────────────────────
    // Ring buffer: [rd_head, rd_tail)
    uint8_t  read_buf[TTY_READ_BUF_SIZE];
    uint32_t rd_head;
    uint32_t rd_tail;

    // ── Canonical line buffer: accumulates until \n or EOF ────────────────
    uint8_t  line_buf[TTY_LINE_BUF_SIZE];
    uint32_t line_len;

    // ── Wait queue ───────────────────────────────────────────────────────
    // Blocking readers register task_we_t nodes here.  poll/epoll
    // waiters register epoll_we_t nodes here.  Every data-arrival
    // event calls wait_queue_wake_all which fires both kinds.
    // SMP-safe: wake_all is lock-free via atomic xchg.
    wait_queue_t   waitq;

    // ── Output ops (tty→screen) ───────────────────────────────────────────
    // Called by the line discipline to echo input or for tty write().
    void (*write_char)(struct tty_t* tty, uint8_t c);

    // Optional batched output: if non-NULL, tty_vfs_write uses this to
    // emit the whole user buffer at once, avoiding per-byte preempt
    // toggles and wake_all storms.  Implementations must still honour
    // the ONLCR '\n' → "\r\n" translation internally.  If NULL, the
    // per-byte write_char path is used.
    void (*write_buf)(struct tty_t* tty, const uint8_t* buf, uint64_t len);

    // ── ANSI escape sequence filter state (canonical mode) ───────────────
    // 0=normal, 1=saw ESC, 2=inside CSI (ESC [)
    uint8_t esc_state;

    // ── input_core handler (registered at tty_init) ───────────────────────
    // Embedded directly in tty_t so no separate allocation is needed.
    input_handler_t input_handler;

    // ── TTY name (e.g. "tty0") ────────────────────────────────────────────
    char name[16];
} tty_t;

// ── Physical console TTY ──────────────────────────────────────────────────
// tty0 = console (framebuffer + PS/2 keyboard).
extern tty_t g_tty0;

// ── API ───────────────────────────────────────────────────────────────────

// Called once at boot (after fb_init and keyboard_init).
void tty_init(void);

// Called from keyboard IRQ handler with a single translated character.
// Runs the N_TTY line discipline: echoes, buffers, signals, wakes readers.
// c=0 is ignored.
void tty_input_char(tty_t* tty, char c);

// Called from keyboard IRQ with a raw scancode byte (before translation).
// Used to detect Ctrl+C, Ctrl+Z at the IRQ level regardless of mode.
// The keyboard driver calls tty_input_char with the translated char;
// signal detection is done inside tty_input_char from c_cc.
// (This function exists for extensibility — e.g. future raw-mode apps.)

// Open /dev/tty or /dev/ttyN → returns a vfs_file_t backed by the tty.
// Each open() returns a fresh vfs_file_t pointing to the same tty_t.
struct vfs_file_t* tty_open(int idx);  // idx 0 = tty0

// Get the tty that is the controlling terminal of the calling process.
// Returns NULL if the process has no controlling terminal.
tty_t* tty_get_ctty(void);

// Assign tty as the controlling terminal of the current session.
void tty_set_ctty(tty_t* tty);

// Flush the read buffer and line buffer (e.g. on TCSETSF).
void tty_flush_input(tty_t* tty);
