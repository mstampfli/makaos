#include "kprintf.h"

// Freestanding kernel — use the compiler's va_list builtins directly rather
// than pulling in <stdarg.h>.
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

static void kp_putc(char c) {
    uint64_t f = serial_lock_irqsave();
    serial_raw_putc(c);
    serial_unlock_irqrestore(f);
}

static void kp_puts(const char* s) {
    if (!s) { kp_puts("(null)"); return; }
    while (*s) kp_putc(*s++);
}

// Emit an unsigned 64-bit value in the given base (10 or 16).  Uppercase
// picks the 'A'-'F' digits for hex.
static void kp_u64(uint64_t v, unsigned base, int upper) {
    char buf[32];
    int n = 0;
    if (v == 0) { kp_putc('0'); return; }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v && n < (int)sizeof(buf)) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n--) kp_putc(buf[n]);
}

// Emit a signed 32-bit decimal (handles INT_MIN correctly via unsigned cast).
static void kp_s32(int32_t v) {
    if (v < 0) { kp_putc('-'); kp_u64((uint64_t)(uint32_t)(-(int64_t)v), 10, 0); }
    else       {                kp_u64((uint64_t)v, 10, 0); }
}

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') { kp_putc(*fmt); continue; }
        fmt++;
        if (!*fmt) break;

        // 'l' length modifier: %lu, %lx, %lX (64-bit).
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (!*fmt) break; }

        switch (*fmt) {
        case 'c': kp_putc((char)va_arg(ap, int)); break;
        case 's': kp_puts(va_arg(ap, const char*)); break;
        case 'd': kp_s32(va_arg(ap, int32_t)); break;
        case 'u':
            if (is_long) kp_u64(va_arg(ap, uint64_t), 10, 0);
            else         kp_u64((uint64_t)va_arg(ap, uint32_t), 10, 0);
            break;
        case 'x':
            if (is_long) kp_u64(va_arg(ap, uint64_t), 16, 0);
            else         kp_u64((uint64_t)va_arg(ap, uint32_t), 16, 0);
            break;
        case 'X':
            if (is_long) kp_u64(va_arg(ap, uint64_t), 16, 1);
            else         kp_u64((uint64_t)va_arg(ap, uint32_t), 16, 1);
            break;
        case 'p': {
            uint64_t p = (uint64_t)va_arg(ap, void*);
            kp_putc('0'); kp_putc('x');
            for (int i = 60; i >= 0; i -= 4) {
                uint8_t n = (uint8_t)((p >> i) & 0xF);
                kp_putc(n < 10 ? '0' + n : 'a' + (n - 10));
            }
            break;
        }
        case '%': kp_putc('%'); break;
        default:  kp_putc('%'); kp_putc(*fmt); break;
        }
    }

    va_end(ap);
}
