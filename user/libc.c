#include "libc.h"

// ── errno ─────────────────────────────────────────────────────────────────
// Global error number — set by syscall wrappers on failure.
int errno = 0;

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
    while (n && (*dst++ = *src++)) n--;
    while (n--) *dst++ = '\0';
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

static int vsnprintf_impl(char* buf, size_t size, const char* fmt, va_list ap) {
    size_t pos = 0;
    char tmp[21];

    for (; *fmt; fmt++) {
        if (*fmt != '%') { buf_putc(buf, size, &pos, *fmt); continue; }
        fmt++;
        if (!*fmt) break;

        int zero_pad = 0, width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        switch (*fmt) {
            case 'd': {
                int64_t v = (int64_t)va_arg(ap, int);
                int neg = (v < 0);
                uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
                int len = u64_to_dec(uv, tmp, 10);
                int pad = width - len - neg;
                if (neg && zero_pad) buf_putc(buf, size, &pos, '-');
                for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                if (neg && !zero_pad) buf_putc(buf, size, &pos, '-');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                break;
            }
            case 'u': {
                uint64_t v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 10);
                int pad = width - len;
                for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                break;
            }
            case 'o': {
                uint64_t v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 8);
                int pad = width - len;
                for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                break;
            }
            case 'x': case 'X': {
                uint64_t v = (uint64_t)va_arg(ap, unsigned int);
                int len = u64_to_dec(v, tmp, 16);
                int pad = width - len;
                for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                buf_puts(buf, size, &pos, tmp, (size_t)len);
                break;
            }
            case 'l': {
                // Only handle %lu, %ld, %lx.
                fmt++;
                if (*fmt == 'u') {
                    uint64_t v = va_arg(ap, uint64_t);
                    int len = u64_to_dec(v, tmp, 10);
                    int pad = width - len;
                    for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                    buf_puts(buf, size, &pos, tmp, (size_t)len);
                } else if (*fmt == 'd') {
                    int64_t v = va_arg(ap, int64_t);
                    int neg = (v < 0);
                    uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
                    int len = u64_to_dec(uv, tmp, 10);
                    int pad = width - len - neg;
                    if (neg && zero_pad) buf_putc(buf, size, &pos, '-');
                    for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                    if (neg && !zero_pad) buf_putc(buf, size, &pos, '-');
                    buf_puts(buf, size, &pos, tmp, (size_t)len);
                } else if (*fmt == 'x') {
                    uint64_t v = va_arg(ap, uint64_t);
                    int len = u64_to_dec(v, tmp, 16);
                    int pad = width - len;
                    for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, zero_pad ? '0' : ' ');
                    buf_puts(buf, size, &pos, tmp, (size_t)len);
                }
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t len = strlen(s);
                int pad = width - (int)len;
                for (int i = 0; i < pad; i++) buf_putc(buf, size, &pos, ' ');
                buf_puts(buf, size, &pos, s, len);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                buf_putc(buf, size, &pos, c);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uint64_t)va_arg(ap, void*);
                buf_puts(buf, size, &pos, "0x", 2);
                int len = u64_to_dec(v, tmp, 16);
                buf_puts(buf, size, &pos, tmp, (size_t)len);
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
