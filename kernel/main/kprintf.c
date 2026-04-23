#include "kprintf.h"

// Freestanding kernel — use the compiler's va_list builtins directly rather
// than pulling in <stdarg.h>.
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

// Output sink: a function pointer + context pointer so the same
// formatter body is reused by every consumer — the per-char-locked
// kprintf, the whole-line-locked kprintf_atomic, and the buffer-
// targetting ksnprintf.  Threading `ctx` through the call chain means
// per-caller state (snprintf's buffer+remaining, future consumers)
// lives on the stack frame of the call, not in a static, so two CPUs
// formatting simultaneously never stomp on each other.
typedef void (*kp_out_t)(char, void*);

static void kp_putc_locked(char c, void* ctx) {
    (void)ctx;
    uint64_t f = serial_lock_irqsave();
    serial_raw_putc(c);
    serial_unlock_irqrestore(f);
}
static void kp_putc_nolock(char c, void* ctx) {
    (void)ctx;
    serial_raw_putc(c);
}

static void kp_puts(kp_out_t out, void* ctx, const char* s) {
    if (!s) { kp_puts(out, ctx, "(null)"); return; }
    while (*s) out(*s++, ctx);
}

// Emit an unsigned 64-bit value in the given base (10 or 16).  Uppercase
// picks the 'A'-'F' digits for hex.
static void kp_u64(kp_out_t out, void* ctx, uint64_t v, unsigned base, int upper) {
    char buf[32];
    int n = 0;
    if (v == 0) { out('0', ctx); return; }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v && n < (int)sizeof(buf)) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n--) out(buf[n], ctx);
}

// Emit a signed 32-bit decimal (handles INT_MIN correctly via unsigned cast).
static void kp_s32(kp_out_t out, void* ctx, int32_t v) {
    if (v < 0) { out('-', ctx); kp_u64(out, ctx, (uint64_t)(uint32_t)(-(int64_t)v), 10, 0); }
    else       {                kp_u64(out, ctx, (uint64_t)v, 10, 0); }
}

// Emit a number with optional zero-padded minimum width.
static void kp_u64_padded(kp_out_t out, void* ctx, uint64_t v, uint8_t base, int upper,
                            int width, int zero_pad) {
    // Render into a local buffer so we know the natural length.
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        while (v) {
            uint8_t d = (uint8_t)(v % base);
            tmp[n++] = (char)(d < 10 ? '0' + d
                                      : (upper ? 'A' : 'a') + (d - 10));
            v /= base;
        }
    }
    for (int i = n; i < width; i++) out(zero_pad ? '0' : ' ', ctx);
    while (n--) out(tmp[n], ctx);
}

// Core formatter — parameterized on the output sink so the same body
// serves kprintf (per-char-locked), kprintf_atomic (outer-lock), and
// ksnprintf (buffer target).  Supports %c, %s, %d, %u, %x/%X, %p, %%
// with optional '0' flag and decimal minimum width.  `l` length
// modifier promotes to 64-bit.
static void kp_format(kp_out_t out, void* ctx, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(*fmt, ctx); continue; }
        fmt++;
        if (!*fmt) break;

        int zero_pad = 0;
        int width    = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; if (!*fmt) break; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
            if (!*fmt) return;
        }

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (!*fmt) break; }

        switch (*fmt) {
        case 'c': out((char)va_arg(ap, int), ctx); break;
        case 's': kp_puts(out, ctx, va_arg(ap, const char*)); break;
        case 'd': kp_s32(out, ctx, va_arg(ap, int32_t)); break;
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, ctx, v, 10, 0, width, zero_pad);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, ctx, v, 16, 0, width, zero_pad);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, ctx, v, 16, 1, width, zero_pad);
            break;
        }
        case 'p': {
            uint64_t p = (uint64_t)va_arg(ap, void*);
            out('0', ctx); out('x', ctx);
            for (int i = 60; i >= 0; i -= 4) {
                uint8_t n = (uint8_t)((p >> i) & 0xF);
                out(n < 10 ? '0' + n : 'a' + (n - 10), ctx);
            }
            break;
        }
        case '%': out('%', ctx); break;
        default:  out('%', ctx); out(*fmt, ctx); break;
        }
    }
}

// Normal kprintf: locks per-char.  Two CPUs calling simultaneously
// may byte-interleave in the output.  Cheap, tolerant of printf-in-
// IRQ; the debug-noise path, not a diagnostic-critical line.
void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kp_format(kp_putc_locked, (void*)0, fmt, ap);
    va_end(ap);
}

// Atomic kprintf: holds the serial lock for the whole format+emit.
// No other kprintf call on any CPU can interleave into the output.
// Use sparingly — extended hold times delay IRQ-handler kprintfs —
// but the right tool for "report a test-run result that must be
// parseable from the serial log."
void kprintf_atomic(const char* fmt, ...) {
    uint64_t f = serial_lock_irqsave();
    va_list ap;
    va_start(ap, fmt);
    kp_format(kp_putc_nolock, (void*)0, fmt, ap);
    va_end(ap);
    serial_unlock_irqrestore(f);
}

/* ── ksnprintf — format into a caller-provided buffer ─────────────────
 *
 * Sink-function pattern reused: same kp_format body, but the sink
 * appends into a buffer instead of the UART.  The per-call state
 * lives in a stack-local snprintf_sink_t reached via the formatter's
 * ctx pointer — fully re-entrant and multi-CPU safe.
 *
 * Return value matches snprintf(3): bytes that WOULD have been
 * written (excl. NUL).  Truncates to (size-1) and always NUL-
 * terminates when size > 0.  No locks, no heap, no IRQ fiddling.
 * Safe from panic/assert/trace-dump paths where the system state is
 * already compromised.
 */
typedef struct {
    char*  p;           /* next write position */
    size_t rem;         /* bytes left before truncation */
    size_t total;       /* grand total of bytes requested */
} snprintf_sink_t;

static void kp_putc_snprintf(char c, void* ctx) {
    snprintf_sink_t* s = (snprintf_sink_t*)ctx;
    s->total++;
    if (s->rem > 1) {  /* leave 1 byte for NUL */
        *s->p++ = c;
        s->rem--;
    }
}

int kvsnprintf(char* buf, size_t size, const char* fmt, __kp_va_list ap) {
    snprintf_sink_t sink = { .p = buf, .rem = size, .total = 0 };
    kp_format(kp_putc_snprintf, &sink, fmt, ap);
    if (size > 0) {
        /* Always NUL-terminate within the caller's buffer. */
        if (sink.rem > 0) *sink.p = 0;
        else              buf[size - 1] = 0;
    }
    return (int)sink.total;
}

int ksnprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
