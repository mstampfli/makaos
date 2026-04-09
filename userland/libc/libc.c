#include "libc.h"

// ── errno ─────────────────────────────────────────────────────────────────
// Global error number — set by syscall wrappers on failure.
int errno = 0;

// ── Signal restorer trampoline ────────────────────────────────────────────
// Called (via `ret`) when a signal handler returns.
// Invokes SYS_SIGRETURN (28) to restore the interrupted user context.
// Must be a real (non-inline) function so its address is stable.
__attribute__((noreturn)) void __sigreturn_trampoline(void) {
    syscall0(SYS_SIGRETURN);
    for (;;);
}

// ── String functions ──────────────────────────────────────────────────────

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* strcpy(char* dst, const char* src) {
    char* r = dst;
    while ((*dst++ = *src++));
    return r;
}

char* strncpy(char* dst, const char* src, size_t n) {
    char* r = dst;
    size_t i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
        if (!src[i]) break;
    }
    for (; i < n; i++) dst[i] = '\0';
    return r;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char* strndup(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    char* p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = '\0'; }
    return p;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

long strtol(const char* s, char** endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    long val = 0;
    const char* start = s;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    (void)start;
    if (endptr) *endptr = (char*)s;
    return neg ? -val : val;
}

long atoi(const char* s) {
    return strtol(s, NULL, 10);
}

// ── malloc / free ─────────────────────────────────────────────────────────
// Simple bump allocator with a free list for recycling freed blocks.
// Uses SYS_BRK to grow the heap.

#define ALIGN8(x)    (((x) + 7u) & ~7u)
#define BLOCK_MAGIC  0xDEADBEEFu

typedef struct block_hdr {
    uint32_t magic;
    uint32_t size;          // usable bytes (not including header)
    uint8_t  free;
    uint8_t  _pad[3];
    struct block_hdr* next_free;
} block_hdr_t;

#define HDR_SIZE ALIGN8(sizeof(block_hdr_t))

static uint64_t s_heap_start = 0;
static uint64_t s_heap_end   = 0;
static block_hdr_t* s_free_list = NULL;

static int heap_grow(size_t need) {
    size_t new_end = s_heap_end + need + 4096;
    new_end = (new_end + 4095u) & ~4095u;
    uint64_t got = brk(new_end);
    if (got == (uint64_t)-1) return 0;
    s_heap_end = got;
    return 1;
}

static block_hdr_t* heap_alloc_raw(size_t size) {
    size_t total = HDR_SIZE + ALIGN8(size);
    if (s_heap_start == 0) {
        s_heap_start = brk(0);
        if (s_heap_start == (uint64_t)-1) return NULL;
        s_heap_end = s_heap_start;
    }
    while (s_heap_end - s_heap_start < total) {
        if (!heap_grow(total)) return NULL;
    }
    block_hdr_t* hdr = (block_hdr_t*)s_heap_start;
    // Bump the start pointer so the next allocation follows.
    // Actually we track end of used space with a separate cursor.
    // Use a static bump pointer instead.
    static uint64_t s_bump = 0;
    if (s_bump == 0) s_bump = s_heap_start;

    if (s_bump + total > s_heap_end) {
        if (!heap_grow(total)) return NULL;
    }

    hdr = (block_hdr_t*)s_bump;
    s_bump += total;

    hdr->magic     = BLOCK_MAGIC;
    hdr->size      = (uint32_t)ALIGN8(size);
    hdr->free      = 0;
    hdr->next_free = NULL;
    return hdr;
}

void* malloc(size_t size) {
    if (!size) size = 1; // POSIX: malloc(0) returns a unique valid pointer
    size = ALIGN8(size);

    // Search free list for a fitting block.
    block_hdr_t** pp = &s_free_list;
    while (*pp) {
        block_hdr_t* b = *pp;
        if (b->magic == BLOCK_MAGIC && b->free && b->size >= size) {
            *pp = b->next_free;
            b->free = 0;
            b->next_free = NULL;
            return (uint8_t*)b + HDR_SIZE;
        }
        pp = &b->next_free;
    }

    block_hdr_t* hdr = heap_alloc_raw(size);
    if (!hdr) return NULL;
    return (uint8_t*)hdr + HDR_SIZE;
}

void free(void* ptr) {
    if (!ptr) return;
    block_hdr_t* hdr = (block_hdr_t*)((uint8_t*)ptr - HDR_SIZE);
    if (hdr->magic != BLOCK_MAGIC || hdr->free) return;
    hdr->free = 1;
    hdr->next_free = s_free_list;
    s_free_list = hdr;
}

void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (!new_size) { free(ptr); return NULL; }

    block_hdr_t* hdr = (block_hdr_t*)((uint8_t*)ptr - HDR_SIZE);
    if (hdr->magic != BLOCK_MAGIC) return NULL;

    if (hdr->size >= ALIGN8(new_size)) return ptr;

    void* new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    size_t copy = hdr->size < new_size ? hdr->size : new_size;
    memcpy(new_ptr, ptr, copy);
    free(ptr);
    return new_ptr;
}

// ── printf / snprintf ─────────────────────────────────────────────────────

// va_list — rely on GCC builtins (work in freestanding with -ffreestanding).
typedef __builtin_va_list va_list;
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)

// Write a string of `len` chars to a fixed buffer, updating pos.
// Returns number of chars that *would* have been written (for snprintf semantics).
static int buf_puts(char* buf, size_t size, size_t* pos, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf && *pos + 1 < size) buf[*pos] = s[i];
        (*pos)++;
    }
    return (int)len;
}

static int buf_putc(char* buf, size_t size, size_t* pos, char c) {
    if (buf && *pos + 1 < size) buf[*pos] = c;
    (*pos)++;
    return 1;
}

// Unsigned decimal to string. Returns length written into tmp (NOT null-terminated).
static int u64_to_dec(uint64_t v, char* tmp, int base) {
    const char* digits = "0123456789abcdef";
    if (v == 0) { tmp[0] = '0'; return 1; }
    int len = 0;
    char rev[20];
    while (v) { rev[len++] = digits[v % (uint64_t)base]; v /= (uint64_t)base; }
    for (int i = 0; i < len; i++) tmp[i] = rev[len - 1 - i];
    return len;
}

// ── Double-to-ASCII (for %f in vsnprintf_impl) ───────────────────────────
// Returns number of chars written into out[]. out must be >= 40 bytes.
// prec: number of digits after the decimal point (default 6).
static int dtoa(char* out, double v, int prec) {
    char* p = out;
    if (prec < 0) prec = 6;
    if (prec > 15) prec = 15;

    // Handle negative.
    if (v < 0.0) { *p++ = '-'; v = -v; }

    // Handle NaN/Inf by bit inspection.
    uint64_t bits;
    __builtin_memcpy(&bits, &v, 8);
    int exp11 = (int)((bits >> 52) & 0x7FF);
    if (exp11 == 0x7FF) {
        const char* s = (bits & 0x000FFFFFFFFFFFFFULL) ? "nan" : "inf";
        while (*s) *p++ = *s++;
        *p = '\0';
        return (int)(p - out);
    }

    // Split into integer and fractional parts.
    // Scale fraction by 10^prec, round, then print both parts.
    // This avoids dependency on log/pow from math.c.
    long long ipart = (long long)v;
    double fpart = v - (double)ipart;

    // Scale fpart: multiply by 10^prec.
    double scale = 1.0;
    for (int i = 0; i < prec; i++) scale *= 10.0;
    long long fscaled = (long long)(fpart * scale + 0.5);

    // Carry if rounding overflowed.
    long long limit = (long long)scale;
    if (fscaled >= limit) { fscaled -= limit; ipart++; }

    // Write integer part.
    char itmp[21];
    int ilen = 0;
    if (ipart == 0) {
        itmp[ilen++] = '0';
    } else {
        long long iv = ipart;
        char rev[21]; int rlen = 0;
        while (iv > 0) { rev[rlen++] = (char)('0' + (iv % 10)); iv /= 10; }
        for (int i = rlen - 1; i >= 0; i--) itmp[ilen++] = rev[i];
    }
    for (int i = 0; i < ilen; i++) *p++ = itmp[i];

    // Write decimal point and fractional digits.
    if (prec > 0) {
        *p++ = '.';
        // Write prec digits from fscaled (may need leading zeros).
        char ftmp[16]; int flen = 0;
        long long fv = fscaled;
        for (int i = 0; i < prec; i++) {
            ftmp[prec - 1 - i] = (char)('0' + (fv % 10));
            fv /= 10;
            flen++;
        }
        for (int i = 0; i < flen; i++) *p++ = ftmp[i];
    }

    *p = '\0';
    return (int)(p - out);
}

static int vsnprintf_impl(char* buf, size_t size, const char* fmt, va_list ap) {
    size_t pos = 0;
    char tmp[48];  // large enough for dtoa output

    for (; *fmt; fmt++) {
        if (*fmt != '%') { buf_putc(buf, size, &pos, *fmt); continue; }
        fmt++;
        if (!*fmt) break;

        // Flags
        int zero_pad  = 0;
        int left_align = 0;
        int plus_sign  = 0;
        int space_sign = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '0') zero_pad   = 1;
            if (*fmt == '-') left_align = 1;
            if (*fmt == '+') plus_sign  = 1;
            if (*fmt == ' ') space_sign = 1;
            fmt++;
        }
        if (left_align) zero_pad = 0;  // POSIX: '-' overrides '0'

        // Width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        // Precision
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        // Length modifier
        int is_long      = 0;
        int is_longlong  = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_longlong = 1; fmt++; }
            else               is_long = 1;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;  // 'hh' — treat as int
        } else if (*fmt == 'z') {
            is_long = 1; fmt++;  // size_t
        }

        switch (*fmt) {
            case 'd': case 'i': {
                int64_t v;
                if (is_longlong)    v = va_arg(ap, int64_t);
                else if (is_long)   v = (int64_t)va_arg(ap, long);
                else                v = (int64_t)va_arg(ap, int);
                int neg = (v < 0);
                uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
                int len = u64_to_dec(uv, tmp, 10);
                // Precision = minimum digits (pad with leading zeros).
                int prec_pad = (prec > len) ? (prec - len) : 0;
                int sign_ch = neg ? '-' : (plus_sign ? '+' : (space_sign ? ' ' : 0));
                int total = len + prec_pad + (sign_ch ? 1 : 0);
                int pad = width - total;
                if (!left_align && !zero_pad) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                if (sign_ch) buf_putc(buf, size, &pos, (char)sign_ch);
                if (!left_align && zero_pad) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, '0');
                for (int i = 0; i < prec_pad; i++) buf_putc(buf, size, &pos, '0');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'u': {
                uint64_t v;
                if (is_longlong)    v = va_arg(ap, uint64_t);
                else if (is_long)   v = (uint64_t)va_arg(ap, unsigned long);
                else                v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 10);
                int prec_pad = (prec > len) ? (prec - len) : 0;
                int total = len + prec_pad;
                int pad = width - total;
                if (!left_align && !zero_pad) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                if (!left_align && zero_pad)  for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, '0');
                for (int i = 0; i < prec_pad; i++) buf_putc(buf, size, &pos, '0');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'o': {
                uint64_t v;
                if (is_longlong)    v = va_arg(ap, uint64_t);
                else if (is_long)   v = (uint64_t)va_arg(ap, unsigned long);
                else                v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 8);
                int prec_pad = (prec > len) ? (prec - len) : 0;
                int total = len + prec_pad;
                int pad = width - total;
                if (!left_align && !zero_pad) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                if (!left_align && zero_pad)  for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, '0');
                for (int i = 0; i < prec_pad; i++) buf_putc(buf, size, &pos, '0');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'x': case 'X': {
                uint64_t v;
                if (is_longlong)    v = va_arg(ap, uint64_t);
                else if (is_long)   v = (uint64_t)va_arg(ap, unsigned long);
                else                v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 16);
                int prec_pad = (prec > len) ? (prec - len) : 0;
                int total = len + prec_pad;
                int pad = width - total;
                if (!left_align && !zero_pad) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                if (!left_align && zero_pad)  for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, '0');
                for (int i = 0; i < prec_pad; i++) buf_putc(buf, size, &pos, '0');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                // Consume double from va_list.
                double v = va_arg(ap, double);
                int len = dtoa(tmp, v, prec);
                int pad = width - len;
                if (!left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t slen = strlen(s);
                if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
                int pad = width - (int)slen;
                if (!left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                buf_puts(buf, size, &pos, s, slen);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                int pad = width - 1;
                if (!left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                buf_putc(buf, size, &pos, c);
                if (left_align) for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void*);
                buf_puts(buf, size, &pos, "0x", 2);
                int len = u64_to_dec(v, tmp, 16);
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                break;
            }
            case 'n': {
                // Store chars written so far into *int.
                int* np = va_arg(ap, int*);
                if (np) *np = (int)pos;
                break;
            }
            case '%': buf_putc(buf, size, &pos, '%'); break;
            default:  buf_putc(buf, size, &pos, '%');
                      buf_putc(buf, size, &pos, *fmt); break;
        }
    }
    if (buf && size > 0) buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf_impl(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    return vsnprintf_impl(buf, size, fmt, ap);
}

// printf: format into a fixed stack buffer then write to fd=1 (stdout).
int printf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf_impl(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    return (int)write(1, buf, (size_t)n);
}

// ── Additional string functions ───────────────────────────────────────────

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

char* strcat(char* dst, const char* src) {
    char* r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return r;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* r = dst;
    while (*dst) dst++;
    while (n && (*dst++ = *src++)) n--;
    if (!n) *dst = '\0';
    return r;
}

static inline int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && to_lower((unsigned char)*a) == to_lower((unsigned char)*b)) {
        a++; b++;
    }
    return to_lower((unsigned char)*a) - to_lower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    while (n && *a && to_lower((unsigned char)*a) == to_lower((unsigned char)*b)) {
        a++; b++; n--;
    }
    if (!n) return 0;
    return to_lower((unsigned char)*a) - to_lower((unsigned char)*b);
}

// sprintf: convenience wrapper around snprintf (no length limit).
int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // Use a very large size — caller must ensure buf is large enough.
    int r = vsnprintf_impl(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

// ── Math helpers ──────────────────────────────────────────────────────────
// abs/labs/llabs: integer absolute value.
int       abs(int x)          { return x < 0 ? -x : x; }
long      labs(long x)        { return x < 0 ? -x : x; }
long long llabs(long long x)  { return x < 0 ? -x : x; }

// ── rand / srand ──────────────────────────────────────────────────────────
// Multiplicative LCG (same multiplier as glibc's simple rand).
static unsigned long long s_rand_state = 1;

void srand(unsigned int seed) {
    s_rand_state = (unsigned long long)seed;
}

int rand(void) {
    s_rand_state = s_rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((s_rand_state >> 33) & 0x7FFFFFFF);
}

// ── qsort ─────────────────────────────────────────────────────────────────
// Iterative Lomuto quicksort using an explicit stack.
// Works for any element size; uses a fixed 256-byte swap buffer.
// Stack depth is O(log n) average; we cap at 128 levels.

static void swap_elems(void* a, void* b, size_t sz) {
    unsigned char tmp[256];
    unsigned char* pa = (unsigned char*)a;
    unsigned char* pb = (unsigned char*)b;
    while (sz > 0) {
        size_t chunk = sz > 256 ? 256 : sz;
        for (size_t i = 0; i < chunk; i++) tmp[i]  = pa[i];
        for (size_t i = 0; i < chunk; i++) pa[i]   = pb[i];
        for (size_t i = 0; i < chunk; i++) pb[i]   = tmp[i];
        pa += chunk; pb += chunk; sz -= chunk;
    }
}

void qsort(void* base, size_t nmemb, size_t size,
           int (*cmp)(const void*, const void*)) {
    if (nmemb < 2 || !size) return;

    // Explicit stack of (lo, hi) index pairs.
#define QSORT_STACK_DEPTH 128
    size_t lo_stk[QSORT_STACK_DEPTH];
    size_t hi_stk[QSORT_STACK_DEPTH];
    int top = 0;

    lo_stk[top] = 0;
    hi_stk[top] = nmemb - 1;

    unsigned char* arr = (unsigned char*)base;

    while (top >= 0) {
        size_t lo = lo_stk[top];
        size_t hi = hi_stk[top--];

        if (lo >= hi) continue;

        // Median-of-three pivot selection to avoid worst-case on sorted input.
        size_t mid = lo + (hi - lo) / 2;
        if (cmp(arr + lo * size, arr + mid * size) > 0)
            swap_elems(arr + lo * size, arr + mid * size, size);
        if (cmp(arr + lo * size, arr + hi * size) > 0)
            swap_elems(arr + lo * size, arr + hi * size, size);
        if (cmp(arr + mid * size, arr + hi * size) > 0)
            swap_elems(arr + mid * size, arr + hi * size, size);
        // Pivot is now at mid; move it to hi-1 for Lomuto partition.
        swap_elems(arr + mid * size, arr + (hi - 1) * size, size);
        void* pivot = arr + (hi - 1) * size;

        // Partition [lo, hi-1) around pivot, with sentinel at hi.
        size_t i = lo;
        for (size_t j = lo; j < hi - 1; j++) {
            if (cmp(arr + j * size, pivot) <= 0) {
                swap_elems(arr + i * size, arr + j * size, size);
                i++;
            }
        }
        swap_elems(arr + i * size, arr + (hi - 1) * size, size);

        // Push sub-partitions (push larger first to minimise stack depth).
        if (top + 2 < QSORT_STACK_DEPTH) {
            if (i > lo + 1) { lo_stk[++top] = lo;    hi_stk[top] = i - 1; }
            if (i + 1 < hi) { lo_stk[++top] = i + 1; hi_stk[top] = hi;    }
        }
    }
#undef QSORT_STACK_DEPTH
}

// ── puts / putchar ─────────────────────────────────────────────────────────
int puts(const char* s) {
    size_t n = strlen(s);
    write(1, s, n);
    write(1, "\n", 1);
    return (int)n + 1;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return (unsigned char)c;
}

// ── sscanf ────────────────────────────────────────────────────────────────
// Minimal sscanf supporting %d, %u, %x, %s, %c, %ld, %lu, %lx, %%, [width].

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    const char* s = str;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            // Width modifier (ignored for now, we parse until whitespace/delimiter).
            while (*fmt >= '0' && *fmt <= '9') fmt++;

            int is_long = 0;
            if (*fmt == 'l') { is_long = 1; fmt++; }

            if (*fmt == '%') {
                if (*s == '%') { s++; }
                fmt++;
                continue;
            }
            if (*fmt == 'c') {
                char* dst = va_arg(ap, char*);
                if (!*s) goto done;
                *dst = *s++;
                count++;
                fmt++;
                continue;
            }
            if (*fmt == 's') {
                char* dst = va_arg(ap, char*);
                // Skip leading whitespace.
                while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                if (!*s) goto done;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n')
                    *dst++ = *s++;
                *dst = '\0';
                count++;
                fmt++;
                continue;
            }
            if (*fmt == 'd' || *fmt == 'i') {
                // Skip whitespace.
                while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                if (!*s) goto done;
                char* end;
                long v = strtol(s, &end, 10);
                if (end == s) goto done;
                if (is_long) *va_arg(ap, long*)       = v;
                else         *va_arg(ap, int*)         = (int)v;
                s = end; count++; fmt++;
                continue;
            }
            if (*fmt == 'u') {
                while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                if (!*s) goto done;
                char* end;
                long v = strtol(s, &end, 10);
                if (end == s) goto done;
                if (is_long) *va_arg(ap, unsigned long*)      = (unsigned long)v;
                else         *va_arg(ap, unsigned int*)        = (unsigned int)v;
                s = end; count++; fmt++;
                continue;
            }
            if (*fmt == 'x' || *fmt == 'X') {
                while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                if (!*s) goto done;
                char* end;
                long v = strtol(s, &end, 16);
                if (end == s) goto done;
                if (is_long) *va_arg(ap, unsigned long*)      = (unsigned long)v;
                else         *va_arg(ap, unsigned int*)        = (unsigned int)v;
                s = end; count++; fmt++;
                continue;
            }
            // Unknown specifier: skip.
            fmt++;
        } else if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            // Whitespace in format matches any amount of whitespace in input.
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            fmt++;
        } else {
            // Literal character match.
            if (*s == *fmt) { s++; }
            else break;
            fmt++;
        }
    }
done:
    va_end(ap);
    return count;
}

// ── time functions ────────────────────────────────────────────────────────

typedef struct { int64_t tv_sec; int64_t tv_usec; } k_timeval_t;

static inline int gettimeofday(k_timeval_t* tv, void* tz) {
    (void)tz;
    return (int)(long)__syscall_ret(syscall2(SYS_GETTOD, (uint64_t)tv, 0));
}

time_t time(time_t* tloc) {
    k_timeval_t tv = {0, 0};
    gettimeofday(&tv, NULL);
    if (tloc) *tloc = (time_t)tv.tv_sec;
    return (time_t)tv.tv_sec;
}

// clock(): milliseconds — CLOCKS_PER_SEC=1000, so clock() returns ms.
clock_t clock(void) {
    return (clock_t)(clock_ns() / 1000000ULL);
}

// ── nanosleep ─────────────────────────────────────────────────────────────

int nanosleep(const timespec_t* req, timespec_t* rem) {
    return (int)(long)__syscall_ret(
        syscall2(SYS_NANOSLEEP, (uint64_t)req, (uint64_t)rem));
}

// ── mmap / munmap ─────────────────────────────────────────────────────────
void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    uint64_t ret;
    register long r8  __asm__("r8")  = (long)fd;
    register long r9  __asm__("r9")  = off;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_MMAP),
          "D"((uint64_t)addr),
          "S"((uint64_t)len),
          "d"((uint64_t)prot),
          "r"((uint64_t)flags),
          "r"(r8),
          "r"(r9)
        : "rcx", "r10", "r11", "memory"
    );
    long r = (long)ret;
    if (r < 0 && r > -4096) { errno = (int)-r; return (void*)-1; }
    return (void*)ret;
}

int munmap(void* addr, size_t len) {
    return (int)(long)__syscall_ret(
        syscall2(SYS_MUNMAP, (uint64_t)addr, (uint64_t)len));
}

// ── strtoul ───────────────────────────────────────────────────────────────
unsigned long strtoul(const char* s, char** endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * (unsigned long)base + (unsigned long)d;
        s++;
    }
    if (endptr) *endptr = (char*)s;
    return val;
}

// ── calloc ────────────────────────────────────────────────────────────────
void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// ── fprintf / vfprintf stubs (until stdio.c provides real FILE*) ──────────
// Defined in stdio.c — forward declaration only here.
