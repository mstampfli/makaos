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

// Core formatter — parameterized on the output sink so the same body
// serves both kprintf (per-char-locked) and kprintf_atomic (outer-lock).
static void kp_format(kp_out_t out, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(*fmt); continue; }
        fmt++;
        if (!*fmt) break;

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (!*fmt) break; }

        switch (*fmt) {
        case 'c': out((char)va_arg(ap, int)); break;
        case 's': kp_puts(out, va_arg(ap, const char*)); break;
        case 'd': kp_s32(out, va_arg(ap, int32_t)); break;
        case 'u':
            if (is_long) kp_u64(out, va_arg(ap, uint64_t), 10, 0);
            else         kp_u64(out, (uint64_t)va_arg(ap, uint32_t), 10, 0);
            break;
        case 'x':
            if (is_long) kp_u64(out, va_arg(ap, uint64_t), 16, 0);
            else         kp_u64(out, (uint64_t)va_arg(ap, uint32_t), 16, 0);
            break;
        case 'X':
            if (is_long) kp_u64(out, va_arg(ap, uint64_t), 16, 1);
            else         kp_u64(out, (uint64_t)va_arg(ap, uint32_t), 16, 1);
            break;
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
