#include "kprintf.h"

// Freestanding kernel — use the compiler's va_list builtins directly rather
// than pulling in <stdarg.h>.
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

// Output sink: a function pointer so the same formatter body can be
// reused by both the per-char-locked kprintf and the whole-line-locked
// kprintf_atomic.  Passing it as an argument (no globals) keeps the
// two variants safe to call concurrently on different CPUs.
typedef void (*kp_out_t)(char);

static void kp_putc_locked(char c) {
    uint64_t f = serial_lock_irqsave();
    serial_raw_putc(c);
    serial_unlock_irqrestore(f);
}
static void kp_putc_nolock(char c) {
    serial_raw_putc(c);
}

static void kp_puts(kp_out_t out, const char* s) {
    if (!s) { kp_puts(out, "(null)"); return; }
    while (*s) out(*s++);
}

// Emit an unsigned 64-bit value in the given base (10 or 16).  Uppercase
// picks the 'A'-'F' digits for hex.
static void kp_u64(kp_out_t out, uint64_t v, unsigned base, int upper) {
    char buf[32];
    int n = 0;
    if (v == 0) { out('0'); return; }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v && n < (int)sizeof(buf)) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n--) out(buf[n]);
}

// Emit a signed 32-bit decimal (handles INT_MIN correctly via unsigned cast).
static void kp_s32(kp_out_t out, int32_t v) {
    if (v < 0) { out('-'); kp_u64(out, (uint64_t)(uint32_t)(-(int64_t)v), 10, 0); }
    else       {           kp_u64(out, (uint64_t)v, 10, 0); }
}

// Emit a number with optional zero-padded minimum width.
static void kp_u64_padded(kp_out_t out, uint64_t v, uint8_t base, int upper,
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
    for (int i = n; i < width; i++) out(zero_pad ? '0' : ' ');
    while (n--) out(tmp[n]);
}

// Core formatter — parameterized on the output sink so the same body
// serves both kprintf (per-char-locked) and kprintf_atomic (outer-lock).
// Supports %c, %s, %d, %u, %x/%X, %p, %% — with optional '0' flag and
// decimal minimum width on the integer conversions.  `l` length modifier
// promotes to 64-bit; nothing else is parsed.
static void kp_format(kp_out_t out, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(*fmt); continue; }
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
        case 'c': out((char)va_arg(ap, int)); break;
        case 's': kp_puts(out, va_arg(ap, const char*)); break;
        case 'd': kp_s32(out, va_arg(ap, int32_t)); break;
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, v, 10, 0, width, zero_pad);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, v, 16, 0, width, zero_pad);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, uint32_t);
            kp_u64_padded(out, v, 16, 1, width, zero_pad);
            break;
        }
        case 'p': {
            uint64_t p = (uint64_t)va_arg(ap, void*);
            out('0'); out('x');
            for (int i = 60; i >= 0; i -= 4) {
                uint8_t n = (uint8_t)((p >> i) & 0xF);
                out(n < 10 ? '0' + n : 'a' + (n - 10));
            }
            break;
        }
        case '%': out('%'); break;
        default:  out('%'); out(*fmt); break;
        }
    }
}

// Normal kprintf: locks per-char.  Two CPUs calling simultaneously
// may byte-interleave in the output.  Cheap, tolerant of printf-in-
// IRQ; the debug-noise path, not a diagnostic-critical line.
void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kp_format(kp_putc_locked, fmt, ap);
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
    kp_format(kp_putc_nolock, fmt, ap);
    va_end(ap);
    serial_unlock_irqrestore(f);
}
