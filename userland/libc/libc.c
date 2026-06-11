#include "libc.h"

// ── errno ─────────────────────────────────────────────────────────────────
// Thread-local — every thread sees its own errno (the global version
// poisoned every multithreaded port: one thread's failed open changed
// another thread's error-handling branch).  Lives in .tbss; crt0 sets
// up the TLS block before anything can touch it.
__thread int errno;

// glibc-ism: argv[0] split into "/path/to/prog" and "prog".  Several
// upstream ports (systemd, libinput, elfutils, util-linux) log
// through these.  Populated from crt0 when available; default to an
// empty string so printf("%s", program_invocation_short_name) is safe.
char  _prog_name_default[]       = "";
char* program_invocation_name       = _prog_name_default;
char* program_invocation_short_name = _prog_name_default;

// ── environ ───────────────────────────────────────────────────────────────
// Null-terminated array of "KEY=VALUE" strings, set by ELF loader.
// If the loader doesn't set it, default to an empty environment.
static char* s_empty_env[] = { NULL };
char** environ = s_empty_env;

// Count entries in environ
static int _env_count(void) {
    int n = 0;
    if (environ) while (environ[n]) n++;
    return n;
}

// Find index of NAME= in environ, returns -1 if not found
static int _env_find(const char* name, size_t nlen) {
    if (!environ) return -1;
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=')
            return i;
    }
    return -1;
}

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t nlen = strlen(name);
    int idx = _env_find(name, nlen);
    if (idx >= 0 && !overwrite) return 0;

    size_t vlen = strlen(value);
    char* entry = malloc(nlen + 1 + vlen + 1);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen + 1);

    if (idx >= 0) {
        environ[idx] = entry;
    } else {
        int n = _env_count();
        char** newenv = malloc((n + 2) * sizeof(char*));
        if (!newenv) { free(entry); errno = ENOMEM; return -1; }
        for (int i = 0; i < n; i++) newenv[i] = environ[i];
        newenv[n] = entry;
        newenv[n + 1] = NULL;
        environ = newenv;
    }
    return 0;
}

int unsetenv(const char* name) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t nlen = strlen(name);
    int idx = _env_find(name, nlen);
    if (idx < 0) return 0;
    int n = _env_count();
    for (int i = idx; i < n; i++) environ[i] = environ[i + 1];
    return 0;
}

int putenv(char* string) {
    if (!string) { errno = EINVAL; return -1; }
    char* eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    size_t nlen = (size_t)(eq - string);
    int idx = _env_find(string, nlen);
    if (idx >= 0) {
        environ[idx] = string;
    } else {
        int n = _env_count();
        char** newenv = malloc((n + 2) * sizeof(char*));
        if (!newenv) { errno = ENOMEM; return -1; }
        for (int i = 0; i < n; i++) newenv[i] = environ[i];
        newenv[n] = string;
        newenv[n + 1] = NULL;
        environ = newenv;
    }
    return 0;
}

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
#define ALIGN16(x)   (((x) + 15u) & ~15u)
#define BLOCK_MAGIC  0xDEADBEEFu

typedef struct block_hdr {
    uint32_t magic;
    uint32_t size;          // usable bytes (not including header)
    uint8_t  free;
    uint8_t  _pad[3];
    struct block_hdr* next_free;
} block_hdr_t;

// Header padded to 16 so a 16-aligned header yields a 16-aligned
// payload — the x86-64 ABI guarantee gcc's vectorizer relies on.
#define HDR_SIZE ALIGN16(sizeof(block_hdr_t))

static uint64_t s_heap_start = 0;
static uint64_t s_heap_end   = 0;
static block_hdr_t* s_free_list = NULL;

// Allocator lock.  foot/fcft/SDL are multithreaded: with an unlocked
// bump allocator, two threads could pass the capacity check together
// and the second write landed past the heap VMA — PF-KILL just above
// heap end.  Spin+yield avoids a pthread.h dependency inside libc.
// realloc/calloc stay unlocked: they compose malloc/free, and the
// header fields they read belong to a block the caller owns.
static volatile unsigned char s_heap_lock_flag = 0;
// Raw yield syscall (SYS_SCHED_YIELD) — sched_yield() the function
// lives in pthread.c, which minimal in-tree binaries don't link.
#define HEAP_SYS_SCHED_YIELD 104
static void heap_lock(void) {
    while (__atomic_test_and_set(&s_heap_lock_flag, __ATOMIC_ACQUIRE))
        (void)syscall0(HEAP_SYS_SCHED_YIELD);
}
static void heap_unlock(void) {
    __atomic_clear(&s_heap_lock_flag, __ATOMIC_RELEASE);
}

static int heap_grow(size_t need) {
    size_t new_end = s_heap_end + need + 4096;
    new_end = (new_end + 4095u) & ~4095u;
    uint64_t got = brk(new_end);
    // brk returns the new break on success and -errno on failure —
    // NEVER -1.  Accept only the exact address we asked for; the old
    // `== -1` check let a -ENOMEM return value through as the "new
    // heap end" and the next allocation indexed unmapped memory.
    if (got != new_end) return 0;
    s_heap_end = got;
    return 1;
}

static block_hdr_t* heap_alloc_raw(size_t size) {
    size_t total = HDR_SIZE + ALIGN16(size);   // every block keeps successors 16-aligned
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
    if (s_bump == 0) s_bump = ALIGN16(s_heap_start);   // headers stay 16-aligned

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

    heap_lock();

    // Search free list for a fitting block.
    block_hdr_t** pp = &s_free_list;
    while (*pp) {
        block_hdr_t* b = *pp;
        if (b->magic == BLOCK_MAGIC && b->free && b->size >= size) {
            *pp = b->next_free;
            b->free = 0;
            b->next_free = NULL;
            heap_unlock();
            return (uint8_t*)b + HDR_SIZE;
        }
        pp = &b->next_free;
    }

    block_hdr_t* hdr = heap_alloc_raw(size);
    heap_unlock();
    if (!hdr) return NULL;
    return (uint8_t*)hdr + HDR_SIZE;
}

void free(void* ptr) {
    if (!ptr) return;
    block_hdr_t* hdr = (block_hdr_t*)((uint8_t*)ptr - HDR_SIZE);
    if (hdr->magic != BLOCK_MAGIC) return;
    heap_lock();
    // The double-free check MUST be under the lock: two threads
    // racing the same pointer could both pass an unlocked check and
    // push the block twice — the list self-links and a later malloc
    // walks garbage.  (Double-free is an app bug, but it must not be
    // able to corrupt the allocator.)
    if (hdr->free) { heap_unlock(); return; }
    hdr->free = 1;
    hdr->next_free = s_free_list;
    s_free_list = hdr;
    heap_unlock();
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

time_t time(time_t* tloc) {
    struct timeval tv = {0, 0};
    gettimeofday(&tv, NULL);
    if (tloc) *tloc = (time_t)tv.tv_sec;
    return (time_t)tv.tv_sec;
}

// clock(): milliseconds — CLOCKS_PER_SEC=1000, so clock() returns ms.
clock_t clock(void) {
    return (clock_t)(clock_ns() / 1000000ULL);
}

// ── nanosleep ─────────────────────────────────────────────────────────────

int nanosleep(const struct timespec* req, struct timespec* rem) {
    return (int)__syscall_ret(
        syscall2(SYS_NANOSLEEP, (uint64_t)req, (uint64_t)rem));
}

// ── usleep — POSIX.1-2001 variant taking microseconds. ───────────────────
// useconds_t max is 1e6.  Split into whole seconds + nanoseconds before
// handing off to SYS_NANOSLEEP.  Returns 0 / -1, sets errno on failure.
int usleep(unsigned us) {
    struct timespec ts = {
        .tv_sec  = (long)(us / 1000000u),
        .tv_nsec = (long)((us % 1000000u) * 1000u),
    };
    return nanosleep(&ts, 0);
}

// ── mmap / munmap ─────────────────────────────────────────────────────────
void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    uint64_t ret;
    register long r10 __asm__("r10") = (long)(uint64_t)flags;
    register long r8  __asm__("r8")  = (long)fd;
    register long r9  __asm__("r9")  = off;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_MMAP),
          "D"((uint64_t)addr),
          "S"((uint64_t)len),
          "d"((uint64_t)prot),
          "r"(r10),
          "r"(r8),
          "r"(r9)
        : "rcx", "r11", "memory"
    );
    long r = (long)ret;
    if (r < 0 && r > -4096) { errno = (int)-r; return (void*)-1; }
    return (void*)ret;
}

int munmap(void* addr, size_t len) {
    return (int)(long)__syscall_ret(
        syscall2(SYS_MUNMAP, (uint64_t)addr, (uint64_t)len));
}

// msync — our kernel has no cached file-backed mmap path, so the
// fsync semantics POSIX requires are already satisfied implicitly.
// Succeed silently so wayland-shm's "flush shared buffer" path works.
// TODO(scalability-debt-ledger-#5): SYS_MSYNC → page-cache writeback
// once the kernel gains a page cache with dirty tracking.
int msync(void* addr, size_t len, int flags) {
    (void)addr; (void)len; (void)flags;
    return 0;
}

// madvise — no backing implementation yet; return 0 like Linux with
// MADV_NORMAL.  Consumers treat failure as advisory only.
// TODO(scalability-debt-ledger-#5): SYS_MADVISE → vma-hint table +
// reclaimer integration.
int madvise(void* addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

// ── POSIX shared memory ─────────────────────────────────────────────────

int shm_open(const char* name, int oflag, int mode) {
    size_t len = 0;
    while (name[len]) len++;
    long ret = (long)syscall4(SYS_SHM_OPEN, (uint64_t)name, (uint64_t)len,
                               (uint64_t)oflag, (uint64_t)mode);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int shm_unlink(const char* name) {
    size_t len = 0;
    while (name[len]) len++;
    return (int)(long)__syscall_ret(
        syscall2(SYS_SHM_UNLINK, (uint64_t)name, (uint64_t)len));
}

// memfd_create — anonymous shared-memory fd (Linux extension).  foot
// and wlroots use it for wl_shm buffer pools and keymap transfer; the
// mkostemp fallback they otherwise compile backs SHM with a regular
// ext2 file, whose huge sparse ftruncate the filesystem rejects.
// Implemented as a uniquely-named shmem object unlinked immediately:
// the fd keeps the object alive, the name disappears.  Sealing flags
// (MFD_ALLOW_SEALING/MFD_NOEXEC_SEAL) are accepted and ignored — no
// F_ADD_SEALS yet; CLOEXEC is irrelevant while exec replaces the fd
// table wholesale.
int memfd_create(const char* name, unsigned int flags) {
    (void)name; (void)flags;
    static volatile unsigned s_memfd_seq = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned seq = __atomic_fetch_add(&s_memfd_seq, 1, __ATOMIC_RELAXED);
        unsigned tag = ((unsigned)getpid() << 16) ^ seq;
        char shm_name[32];
        int i = 0;
        shm_name[i++] = '/'; shm_name[i++] = 'm'; shm_name[i++] = 'f';
        shm_name[i++] = 'd'; shm_name[i++] = ':';
        for (int d = 7; d >= 0; d--)
            shm_name[i++] = "0123456789abcdef"[(tag >> (d * 4)) & 0xF];
        shm_name[i] = '\0';
        int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(shm_name);
            return fd;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

// ── Framebuffer mapping ──────────────────────────────────────────────────

void* fb_map(void) {
    uint64_t r = syscall0(SYS_FB_MAP);
    long v = (long)r;
    if (v < 0 && v > -4096) { errno = (int)-v; return (void*)-1L; }
    return (void*)r;
}

// ── Pseudo-terminal ─────────────────────────────────────────────────────

int openpty(int fds[2]) {
    return (int)(long)__syscall_ret(syscall1(93 /* SYS_OPENPTY */, (uint64_t)fds));
}

int getpeerpid(int fd) {
    return (int)(long)__syscall_ret(syscall1(94 /* SYS_GETPEERPID */, (uint64_t)fd));
}

// ── BSD Sockets ──────────────────────────────────────────────────────────

int socket(int domain, int type, int protocol) {
    return (int)(long)__syscall_ret(
        syscall3(SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol));
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return (int)(long)__syscall_ret(
        syscall3(SYS_BIND, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}

int listen(int fd, int backlog) {
    return (int)(long)__syscall_ret(
        syscall2(SYS_LISTEN, (uint64_t)fd, (uint64_t)backlog));
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return (int)(long)__syscall_ret(
        syscall3(SYS_ACCEPT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return (int)(long)__syscall_ret(
        syscall3(SYS_CONNECT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen));
}

ssize_t send(int fd, const void* buf, size_t len, int flags) {
    // SYS_SENDTO takes 6 args (fd, buf, len, flags, addr, addrlen).  We
    // must explicitly zero the addr/addrlen slots so the kernel does not
    // read stale r8/r9 values from the caller's register file.
    return (ssize_t)__syscall_ret(
        syscall6(SYS_SENDTO, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                  (uint64_t)flags, 0, 0));
}

ssize_t recv(int fd, void* buf, size_t len, int flags) {
    return (ssize_t)__syscall_ret(
        syscall6(SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                  (uint64_t)flags, 0, 0));
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags,
                const struct sockaddr* dst, socklen_t dstlen) {
    return (ssize_t)__syscall_ret(
        syscall6(SYS_SENDTO, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                  (uint64_t)flags, (uint64_t)dst, (uint64_t)dstlen));
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                  struct sockaddr* src, socklen_t* srclen) {
    return (ssize_t)__syscall_ret(
        syscall6(SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                  (uint64_t)flags, (uint64_t)src, (uint64_t)srclen));
}

int setsockopt(int fd, int level, int optname,
                const void* optval, socklen_t optlen) {
    return (int)__syscall_ret(
        syscall5(SYS_SETSOCKOPT, (uint64_t)fd, (uint64_t)level,
                  (uint64_t)optname, (uint64_t)optval, (uint64_t)optlen));
}

int net_ifconfig(const ifcfg_t* cfg) {
    return (int)__syscall_ret(
        syscall2(SYS_NET_IFCONFIG, (uint64_t)cfg, (uint64_t)sizeof(*cfg)));
}

int net_mac(uint8_t out[6]) {
    return (int)__syscall_ret(syscall1(SYS_NET_MAC, (uint64_t)out));
}

// ── inet_pton / inet_ntop — IPv4 only ────────────────────────────────────
int inet_pton(int family, const char* src, void* out) {
    if (family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (!src || !out) return 0;

    uint32_t parts[4] = {0,0,0,0};
    int      idx = 0;
    int      seen_digit = 0;
    const char* p = src;

    while (*p) {
        if (*p >= '0' && *p <= '9') {
            uint32_t v = (uint32_t)(parts[idx] * 10u + (uint32_t)(*p - '0'));
            if (v > 255) return 0;
            parts[idx] = v;
            seen_digit = 1;
        } else if (*p == '.') {
            if (!seen_digit) return 0;
            idx++;
            if (idx > 3) return 0;
            seen_digit = 0;
        } else {
            return 0;
        }
        p++;
    }
    if (idx != 3 || !seen_digit) return 0;

    uint8_t* dst = (uint8_t*)out;
    dst[0] = (uint8_t)parts[0];
    dst[1] = (uint8_t)parts[1];
    dst[2] = (uint8_t)parts[2];
    dst[3] = (uint8_t)parts[3];
    return 1;
}

const char* inet_ntop(int family, const void* src, char* dst, socklen_t dst_len) {
    if (family != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    if (!src || !dst) { errno = EINVAL; return NULL; }

    const uint8_t* ip = (const uint8_t*)src;
    char tmp[16];   // "255.255.255.255\0"
    int  n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        if (v >= 100) { tmp[n++] = (char)('0' + v / 100); v %= 100;
                         tmp[n++] = (char)('0' + v / 10);  v %= 10;
                         tmp[n++] = (char)('0' + v); }
        else if (v >= 10) { tmp[n++] = (char)('0' + v / 10); v %= 10;
                             tmp[n++] = (char)('0' + v); }
        else tmp[n++] = (char)('0' + v);
        if (i < 3) tmp[n++] = '.';
    }
    tmp[n++] = '\0';

    if ((socklen_t)n > dst_len) { errno = ENOSPC; return NULL; }
    for (int i = 0; i < n; i++) dst[i] = tmp[i];
    return dst;
}

int shutdown(int fd, int how) {
    return (int)(long)__syscall_ret(
        syscall2(SYS_SHUTDOWN, (uint64_t)fd, (uint64_t)how));
}

int sendfd(int sock_fd, int target_fd, unsigned int rights) {
    return (int)(long)__syscall_ret(
        syscall3(SYS_SENDFD, (uint64_t)sock_fd, (uint64_t)target_fd,
                  (uint64_t)rights));
}

int recvfd(int sock_fd) {
    long ret = (long)syscall1(SYS_RECVFD, (uint64_t)sock_fd);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
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

// ── Additional string functions ───────────────────────────────────────────

char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        const char* a = accept;
        while (*a) { if (*s == *a) return (char*)s; a++; }
        s++;
    }
    return NULL;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n]) {
        const char* a = accept;
        int found = 0;
        while (*a) { if (s[n] == *a++) { found = 1; break; } }
        if (!found) break;
        n++;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n]) {
        const char* r = reject;
        while (*r) { if (s[n] == *r) return n; r++; }
        n++;
    }
    return n;
}

char* strtok_r(char* s, const char* delim, char** saveptr) {
    if (!s) s = *saveptr;
    s += strspn(s, delim);
    if (!*s) { *saveptr = s; return NULL; }
    char* tok = s;
    s += strcspn(s, delim);
    if (*s) { *s++ = '\0'; }
    *saveptr = s;
    return tok;
}

static char* s_strtok_save = NULL;
char* strtok(char* s, const char* delim) {
    return strtok_r(s, delim, &s_strtok_save);
}

char* stpcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
    return dst - 1;
}

char* stpncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    char* end = dst + i;
    for (; i < n; i++) dst[i] = '\0';
    return end;
}

char* strchrnul(const char* s, int c) {
    while (*s && *s != (char)c) s++;
    return (char*)s;
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcoll(const char* a, const char* b) {
    return strcmp(a, b);  // C locale — lexicographic order
}

char* strcasestr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack, *n = needle;
        while (*h && *n && ((*h | 32) == (*n | 32))) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

double strtod(const char* s, char** endptr) {
    while (*s == ' ' || *s == '\t') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; } else if (*s == '+') s++;
    double v = 0.0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * frac; frac *= 0.1; s++; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; } else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        double p = 1.0;
        while (exp-- > 0) p *= 10.0;
        if (esign > 0) v *= p; else v /= p;
    }
    if (endptr) *endptr = (char*)s;
    return v * sign;
}

long long strtoll(const char* s, char** endptr, int base) {
    return (long long)strtol(s, endptr, base);
}

unsigned long long strtoull(const char* s, char** endptr, int base) {
    return (unsigned long long)strtoul(s, endptr, base);
}

int asprintf(char** strp, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    *strp = malloc((size_t)n + 1);
    if (!*strp) return -1;
    va_start(ap, fmt);
    vsnprintf(*strp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return n;
}

int vasprintf(char** strp, const char* fmt, __builtin_va_list ap) {
    // measure
    va_list ap2;
    __builtin_va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    __builtin_va_end(ap2);
    if (n < 0) return -1;
    *strp = malloc((size_t)n + 1);
    if (!*strp) return -1;
    vsnprintf(*strp, (size_t)n + 1, fmt, ap);
    return n;
}

// ── POSIX DIR* API ────────────────────────────────────────────────────────
// We store all entries from a single kernel readdir() call in a heap buffer.
// readdir(DIR*) returns one entry at a time by incrementing dir->pos.

DIR* opendir(const char* path) {
    if (!path) { errno = EINVAL; return NULL; }
    size_t plen = strnlen(path, 511);
    k_dirent_t* entries = malloc(4096 * sizeof(k_dirent_t));
    if (!entries) { errno = ENOMEM; return NULL; }
    int count = _sys_readdir(path, plen, entries, 4096);
    if (count < 0) {
        free(entries);
        return NULL;
    }
    DIR* dir = malloc(sizeof(DIR));
    if (!dir) { free(entries); errno = ENOMEM; return NULL; }
    size_t i = 0;
    while (i < 511 && path[i]) { dir->path[i] = path[i]; i++; }
    dir->path[i] = '\0';
    dir->entries = entries;
    dir->count   = count;
    dir->pos     = 0;
    return dir;
}

struct dirent* readdir(DIR* dirp) {
    if (!dirp) { errno = EBADF; return NULL; }
    if (dirp->pos >= dirp->count) return NULL;
    k_dirent_t* ke = &dirp->entries[dirp->pos++];
    dirp->cur.d_ino    = ke->inode_num;
    /* d_off is the byte offset to the NEXT dirent.  Our readdir
     * preloads the whole directory into entries[], so the concept
     * of a file offset doesn't apply — most Linux libcs return the
     * index-of-next-entry here and callers treat it as opaque.
     * Setting it to dirp->pos keeps the contract. */
    dirp->cur.d_off    = (long)dirp->pos;
    dirp->cur.d_reclen = sizeof(struct dirent);
    dirp->cur.d_type   = ke->is_dir ? DT_DIR : DT_REG;
    size_t i = 0;
    while (i < 255 && ke->name[i]) { dirp->cur.d_name[i] = ke->name[i]; i++; }
    dirp->cur.d_name[i] = '\0';
    return &dirp->cur;
}

int closedir(DIR* dirp) {
    if (!dirp) { errno = EBADF; return -1; }
    free(dirp->entries);
    free(dirp);
    return 0;
}

void rewinddir(DIR* dirp) {
    if (dirp) dirp->pos = 0;
}

// ── libgen.h — basename / dirname ────────────────────────────────────────
// POSIX basename/dirname MAY modify the path buffer and return a
// pointer into it (or a static ".") — libinput's quirks.c relies on
// that contract.  Empty path -> ".".  Trailing slashes are stripped.
static char s_dot[] = ".";
static char s_slash[] = "/";

char* basename(char* path) {
    if (!path || !*path) return s_dot;
    // Strip trailing slashes (but not the root "/" itself).
    size_t n = 0; while (path[n]) n++;
    while (n > 1 && path[n - 1] == '/') { path[n - 1] = '\0'; n--; }
    const char* last = path;
    for (size_t i = 0; i < n; i++) if (path[i] == '/') last = path + i + 1;
    return (char*)last;
}

char* dirname(char* path) {
    if (!path || !*path) return s_dot;
    size_t n = 0; while (path[n]) n++;
    // Trailing slashes (keep "/" as "/").
    while (n > 1 && path[n - 1] == '/') { path[n - 1] = '\0'; n--; }
    // Find last slash.
    ssize_t last = -1;
    for (size_t i = 0; i < n; i++) if (path[i] == '/') last = (ssize_t)i;
    if (last < 0) return s_dot;
    if (last == 0) return s_slash;
    path[last] = '\0';
    // Strip again if inner slashes (/a//b → /a).
    while (last > 1 && path[last - 1] == '/') path[--last] = '\0';
    return path;
}

// ── rindex — BSD alias for strrchr, still used by libinput. ───────────────
char* rindex(const char* s, int c) { return strrchr(s, c); }
char* index (const char* s, int c) { return strchr (s, c); }


// locale_t forward-declared here; <locale.h> defines it as a
// pointer to an opaque __locale_struct.  libc.h doesn't include
// <locale.h> since most of the kernel/TUs have no locale concept,
// so introduce the typedef locally.
typedef struct __locale_struct* locale_t;

// ── _l locale-aware parsers — ignore locale, MakaOS is C-only. ───────────
double strtod_l(const char* s, char** endptr, locale_t loc) {
    (void)loc; return strtod(s, endptr);
}
long strtol_l(const char* s, char** endptr, int base, locale_t loc) {
    (void)loc; return strtol(s, endptr, base);
}
unsigned long strtoul_l(const char* s, char** endptr, int base, locale_t loc) {
    (void)loc; return strtoul(s, endptr, base);
}

// ── scandir / alphasort — POSIX directory listing helpers ─────────────────
// libinput scans /dev/input/ for eventN nodes via scandir.  We keep the
// layout POSIX-standard (namelist is an array of malloc'd struct dirent
// pointers — caller frees each + the array).
int alphasort(const struct dirent** a, const struct dirent** b) {
    const char* sa = (*a)->d_name;
    const char* sb = (*b)->d_name;
    while (*sa && *sa == *sb) { sa++; sb++; }
    return (unsigned char)*sa - (unsigned char)*sb;
}

// versionsort: compare filenames with embedded decimal numbers treated
// as integers (event2 < event10).  Matches glibc/musl semantics.
int versionsort(const struct dirent** a, const struct dirent** b) {
    const char* sa = (*a)->d_name;
    const char* sb = (*b)->d_name;
    while (*sa && *sb) {
        if (*sa >= '0' && *sa <= '9' && *sb >= '0' && *sb <= '9') {
            unsigned long na = 0, nb = 0;
            while (*sa >= '0' && *sa <= '9') { na = na * 10 + (*sa - '0'); sa++; }
            while (*sb >= '0' && *sb <= '9') { nb = nb * 10 + (*sb - '0'); sb++; }
            if (na != nb) return na < nb ? -1 : 1;
        } else {
            if (*sa != *sb) return (unsigned char)*sa - (unsigned char)*sb;
            sa++; sb++;
        }
    }
    return (unsigned char)*sa - (unsigned char)*sb;
}

int scandir(const char* path, struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**)) {
    if (!path || !namelist) { errno = EINVAL; return -1; }
    DIR* d = opendir(path);
    if (!d) return -1;

    struct dirent** arr = NULL;
    size_t cap = 0, n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (filter && !filter(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            struct dirent** na = realloc(arr, cap * sizeof(*arr));
            if (!na) { closedir(d); for (size_t i = 0; i < n; i++) free(arr[i]); free(arr); return -1; }
            arr = na;
        }
        struct dirent* copy = malloc(sizeof(*copy));
        if (!copy) { closedir(d); for (size_t i = 0; i < n; i++) free(arr[i]); free(arr); return -1; }
        *copy = *e;
        arr[n++] = copy;
    }
    closedir(d);

    if (compar && n > 1) {
        // Simple insertion sort — N is tiny for /dev/input.
        for (size_t i = 1; i < n; i++) {
            struct dirent* key = arr[i];
            size_t j = i;
            while (j > 0 && compar((const struct dirent**)&arr[j-1],
                                    (const struct dirent**)&key) > 0) {
                arr[j] = arr[j-1]; j--;
            }
            arr[j] = key;
        }
    }

    *namelist = arr;
    return (int)n;
}

// ── passwd database ───────────────────────────────────────────────────────
// Parse /etc/passwd: username:x:uid:gid:gecos:home:shell

static struct passwd s_pw = {0};
static char s_pw_buf[512];

static struct passwd* pw_parse_line(char* line) {
    // Format: name:passwd:uid:gid:gecos:home:shell
    char* fields[7];
    int f = 0;
    char* p = line;
    while (f < 7) {
        fields[f++] = p;
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p) *p++ = '\0';
        else break;
    }
    if (f < 7) return NULL;
    s_pw.pw_name   = fields[0];
    s_pw.pw_passwd = fields[1];
    s_pw.pw_uid    = (uid_t)strtol(fields[2], NULL, 10);
    s_pw.pw_gid    = (gid_t)strtol(fields[3], NULL, 10);
    s_pw.pw_gecos  = fields[4];
    s_pw.pw_dir    = fields[5];
    s_pw.pw_shell  = fields[6];
    return &s_pw;
}

struct passwd* getpwuid(uid_t uid) {
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return NULL;
    int n = read(fd, s_pw_buf, sizeof(s_pw_buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    s_pw_buf[n] = '\0';
    char* line = s_pw_buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next = '\0';
        // make a copy for parsing (pw_parse_line modifies in place)
        char tmp[256]; size_t li = 0;
        while (li < 255 && line[li]) { tmp[li] = line[li]; li++; } tmp[li] = '\0';
        struct passwd* pw = pw_parse_line(tmp);
        if (pw && pw->pw_uid == uid) {
            // copy strings back into s_pw_buf-based storage
            pw_parse_line(line);  // parse directly into line (already NUL'd)
            if (next) *next = '\n';
            return &s_pw;
        }
        if (next) { *next = '\n'; line = next + 1; } else break;
    }
    return NULL;
}

struct passwd* getpwnam(const char* name) {
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return NULL;
    int n = read(fd, s_pw_buf, sizeof(s_pw_buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    s_pw_buf[n] = '\0';
    char* line = s_pw_buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next = '\0';
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            int match = (strcmp(line, name) == 0);
            *colon = ':';
            if (match) {
                pw_parse_line(line);
                if (next) *next = '\n';
                return &s_pw;
            }
        }
        if (next) { *next = '\n'; line = next + 1; } else break;
    }
    return NULL;
}

// ── setpwent / getpwent / endpwent — iterating /etc/passwd ───────────────
// POSIX requires these to iterate through the password database.
// setpwent() opens/rewinds, getpwent() returns next entry, endpwent() closes.
static int    s_pw_fd  = -1;
static char   s_pw_iter_buf[4096]; // full file buffer for iteration
static int    s_pw_iter_len = 0;
static char*  s_pw_iter_pos = NULL;

void setpwent(void) {
    if (s_pw_fd >= 0) close(s_pw_fd);
    s_pw_fd = open("/etc/passwd", O_RDONLY, 0);
    if (s_pw_fd >= 0) {
        s_pw_iter_len = (int)read(s_pw_fd, s_pw_iter_buf, sizeof(s_pw_iter_buf) - 1);
        close(s_pw_fd);
        s_pw_fd = -1;
        if (s_pw_iter_len > 0) {
            s_pw_iter_buf[s_pw_iter_len] = '\0';
            s_pw_iter_pos = s_pw_iter_buf;
        } else {
            s_pw_iter_pos = NULL;
        }
    } else {
        s_pw_iter_pos = NULL;
    }
}

struct passwd* getpwent(void) {
    if (!s_pw_iter_pos || !*s_pw_iter_pos) return NULL;
    char* line = s_pw_iter_pos;
    char* next = strchr(line, '\n');
    if (next) *next = '\0';
    // Skip blank lines
    if (!*line) {
        s_pw_iter_pos = next ? next + 1 : NULL;
        return getpwent();
    }
    struct passwd* pw = pw_parse_line(line);
    s_pw_iter_pos = next ? next + 1 : NULL;
    return pw;
}

void endpwent(void) {
    s_pw_iter_pos = NULL;
    s_pw_iter_len = 0;
}

// ── sysconf / pathconf / confstr ──────────────────────────────────────────
long sysconf(int name) {
    switch (name) {
    case _SC_CLK_TCK:    return 100;
    case _SC_OPEN_MAX:   return 1024;
    case _SC_PAGESIZE:   return 4096;
    case _SC_NGROUPS_MAX:return 32;
    case _SC_ARG_MAX:    return 65536;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        // Real CPU count from the kernel.  Returning -1 here made
        // foot's u16 worker count wrap to 65535 render threads.
        long n = (long)syscall0(113 /* SYS_NPROC */);
        return n > 0 ? n : 1;
    }
    default:             return -1;
    }
}
long pathconf(const char* path, int name) { (void)path; (void)name; return -1; }
int  confstr(int name, char* buf, size_t len) { (void)name; (void)buf; (void)len; return 0; }

// ── mkstemp / mktemp ─────────────────────────────────────────────────────
int mkstemp(char* tmpl) {
    if (mkdtemp_r(tmpl) < 0) return -1;
    int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    return fd;
}
char* mktemp(char* tmpl) {
    if (mkdtemp_r(tmpl) < 0) return NULL;
    return tmpl;
}
int mkostemp(char* tmpl, int flags) {
    if (mkdtemp_r(tmpl) < 0) return -1;
    return open(tmpl, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
}
char* mkdtemp(char* tmpl) {
    if (mkdtemp_r(tmpl) < 0) return NULL;
    if (mkdir(tmpl, 0700) < 0) return NULL;
    return tmpl;
}

// ── alarm ─────────────────────────────────────────────────────────────────
// Stub: no preemptive timer support yet; deliver SIGALRM after `seconds`.
// Returns previous alarm remaining (always 0 for now).
unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    // TODO: wire to kernel setitimer when available
    return 0;
}

// ── abort ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void abort(void) {
    kill(getpid(), SIGABRT);
    for (;;) _exit(1);
}

// ── strerror ─────────────────────────────────────────────────────────────
char* strerror(int e) {
    switch (e) {
    case 0:           return "Success";
    case EPERM:       return "Operation not permitted";
    case ENOENT:      return "No such file or directory";
    case ESRCH:       return "No such process";
    case EINTR:       return "Interrupted system call";
    case EIO:         return "Input/output error";
    case EBADF:       return "Bad file descriptor";
    case ECHILD:      return "No child processes";
    case EAGAIN:      return "Resource temporarily unavailable";
    case ENOMEM:      return "Cannot allocate memory";
    case EACCES:      return "Permission denied";
    case EEXIST:      return "File exists";
    case ENOTDIR:     return "Not a directory";
    case EISDIR:      return "Is a directory";
    case EINVAL:      return "Invalid argument";
    case ENFILE:      return "Too many open files in system";
    case ENOSPC:      return "No space left on device";
    case EPIPE:       return "Broken pipe";
    case ERANGE:      return "Numerical result out of range";
    case ENOTEMPTY:   return "Directory not empty";
    case ENOSYS:      return "Function not implemented";
    case ENOEXEC:     return "Exec format error";
    case EFAULT:      return "Bad address";
    case EBUSY:       return "Device or resource busy";
    case EMFILE:      return "Too many open files";
    case ENOTTY:      return "Not a typewriter";
    case EFBIG:       return "File too large";
    case ESPIPE:      return "Illegal seek";
    case EDEADLK:     return "Resource deadlock avoided";
    case ENAMETOOLONG:return "File name too long";
    case ELOOP:       return "Too many levels of symbolic links";
    // EWOULDBLOCK == EAGAIN, no separate case needed
    case ENOTSOCK:    return "Socket operation on non-socket";
    case EOPNOTSUPP:  return "Operation not supported";
    case EAFNOSUPPORT:return "Address family not supported";
    case EADDRINUSE:  return "Address already in use";
    case ECONNRESET:  return "Connection reset by peer";
    case ENOBUFS:     return "No buffer space available";
    case EISCONN:     return "Transport endpoint already connected";
    case ENOTCONN:    return "Transport endpoint not connected";
    case ETIMEDOUT:   return "Connection timed out";
    case EILSEQ:      return "Invalid or incomplete multibyte character";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "Unknown error %d", e);
        return buf;
    }
    }
}

// ── strsignal ─────────────────────────────────────────────────────────────
char* strsignal(int sig) {
    switch (sig) {
    case SIGHUP:   return "Hangup";
    case SIGINT:   return "Interrupt";
    case SIGQUIT:  return "Quit";
    case SIGILL:   return "Illegal instruction";
    case SIGABRT:  return "Aborted";
    case SIGBUS:   return "Bus error";
    case SIGFPE:   return "Floating point exception";
    case SIGKILL:  return "Killed";
    case SIGUSR1:  return "User defined signal 1";
    case SIGSEGV:  return "Segmentation fault";
    case SIGUSR2:  return "User defined signal 2";
    case SIGPIPE:  return "Broken pipe";
    case SIGALRM:  return "Alarm clock";
    case SIGTERM:  return "Terminated";
    case SIGCHLD:  return "Child exited";
    case SIGCONT:  return "Continued";
    case SIGTSTP:  return "Stopped";
    case SIGTTIN:  return "Stopped (tty input)";
    case SIGTTOU:  return "Stopped (tty output)";
    case SIGWINCH: return "Window size changed";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "Signal %d", sig);
        return buf;
    }
    }
}

// ── strftime / localtime ──────────────────────────────────────────────────
//
// localtime()/gmtime() return a pointer to a shared static buffer per
// POSIX — not thread-safe by design.  New code should prefer the _r
// variants which take a caller-owned tm.  MakaOS is UTC-only; no DST.

static void tm_decompose(time_t t, struct tm* out) {
    long long ts = (long long)t;
    long long day = ts / 86400;
    long long sec = ts % 86400;
    out->tm_sec  = (int)(sec % 60);
    out->tm_min  = (int)((sec / 60) % 60);
    out->tm_hour = (int)(sec / 3600);
    // Days since epoch (Jan 1 1970 = Thursday, wday=4)
    out->tm_wday = (int)((day + 4) % 7);
    // Year/month/day from day count
    long long y = 1970; long long d = day;
    while (1) {
        long long yd = ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365);
        if (d < yd) break;
        d -= yd; y++;
    }
    out->tm_year = (int)(y - 1900);
    out->tm_yday = (int)d;
    int is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    while (m < 12) {
        int dm = mdays[m] + (m == 1 && is_leap ? 1 : 0);
        if (d < dm) break;
        d -= dm; m++;
    }
    out->tm_mon   = m;
    out->tm_mday  = (int)d + 1;
    out->tm_isdst = 0;
}

struct tm* localtime_r(const time_t* t, struct tm* tm) {
    if (!t || !tm) return 0;
    tm_decompose(*t, tm);
    return tm;
}
struct tm* gmtime_r(const time_t* t, struct tm* tm) {
    return localtime_r(t, tm);  // MakaOS is UTC-only
}

static struct tm s_tm;
struct tm* localtime(const time_t* t) {
    if (!t) return 0;
    tm_decompose(*t, &s_tm);
    return &s_tm;
}

struct tm* gmtime(const time_t* t) { return localtime(t); }

size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm) {
    if (!max) return 0;
    size_t pos = 0;
    for (; *fmt && pos + 1 < max; fmt++) {
        if (*fmt != '%') { s[pos++] = *fmt; continue; }
        fmt++;
        char tmp[32]; int n = 0;
        switch (*fmt) {
        case 'Y': n = snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900); break;
        case 'y': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
        case 'm': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'H': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'M': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': n = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'A': { static const char* d[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
                    n = snprintf(tmp, sizeof(tmp), "%s", d[tm->tm_wday % 7]); break; }
        case 'a': { static const char* d[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                    n = snprintf(tmp, sizeof(tmp), "%s", d[tm->tm_wday % 7]); break; }
        case 'B': { static const char* mo[]={"January","February","March","April","May","June",
                      "July","August","September","October","November","December"};
                    n = snprintf(tmp, sizeof(tmp), "%s", mo[tm->tm_mon % 12]); break; }
        case 'b': case 'h': {
                    static const char* mo[]={"Jan","Feb","Mar","Apr","May","Jun",
                      "Jul","Aug","Sep","Oct","Nov","Dec"};
                    n = snprintf(tmp, sizeof(tmp), "%s", mo[tm->tm_mon % 12]); break; }
        case 'j': n = snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
        case 'n': tmp[0] = '\n'; tmp[1] = '\0'; n = 1; break;
        case 't': tmp[0] = '\t'; tmp[1] = '\0'; n = 1; break;
        case '%': tmp[0] = '%'; tmp[1] = '\0'; n = 1; break;
        default:  tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; n = 2; break;
        }
        for (int i = 0; i < n && pos + 1 < max; i++) s[pos++] = tmp[i];
    }
    s[pos] = '\0';
    return pos;
}

// ── fnmatch ───────────────────────────────────────────────────────────────
// Supports *, ?, [...] with FNM_PATHNAME (/ not matched by *) and FNM_CASEFOLD.
static int fnmatch_impl(const char* pat, const char* str, int flags,
                        int depth, const char* str_start) {
    if (depth > 64) return FNM_NOMATCH;  // recursion guard
    while (*pat) {
        if (*pat == '*') {
            // Skip consecutive stars
            while (*pat == '*') pat++;
            if (!*pat) {
                // Trailing *: matches rest unless FNM_PATHNAME and '/' present
                if (flags & FNM_PATHNAME) return strchr(str, '/') ? FNM_NOMATCH : 0;
                return 0;
            }
            // Try matching pat at every position in str
            while (*str) {
                if (flags & FNM_PATHNAME && *str == '/') return FNM_NOMATCH;
                if (fnmatch_impl(pat, str, flags, depth + 1, str_start) == 0) return 0;
                str++;
            }
            return fnmatch_impl(pat, str, flags, depth + 1, str_start);
        }
        if (!*str) return FNM_NOMATCH;
        if (*pat == '?') {
            if (flags & FNM_PATHNAME && *str == '/') return FNM_NOMATCH;
            if (flags & FNM_PERIOD && *str == '.' && str == str_start) return FNM_NOMATCH;
            pat++; str++; continue;
        }
        if (*pat == '[') {
            pat++;
            int invert = 0;
            if (*pat == '!' || *pat == '^') { invert = 1; pat++; }
            int matched = 0;
            const char* bracket_start = pat;
            while (*pat && (*pat != ']' || pat == bracket_start)) {
                if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                    char lo = pat[0], hi = pat[2];
                    char sc = *str;
                    if (flags & FNM_CASEFOLD) {
                        lo = (char)tolower((unsigned char)lo);
                        hi = (char)tolower((unsigned char)hi);
                        sc = (char)tolower((unsigned char)sc);
                    }
                    if (sc >= lo && sc <= hi) matched = 1;
                    pat += 3;
                } else {
                    char pc = *pat, sc = *str;
                    if (flags & FNM_CASEFOLD) {
                        pc = (char)tolower((unsigned char)pc);
                        sc = (char)tolower((unsigned char)sc);
                    }
                    if (pc == sc) matched = 1;
                    pat++;
                }
                if (*pat == ']' && pat[-1] != '[') break;
                (void)bracket_start;
            }
            if (*pat == ']') pat++;
            if (matched == invert) return FNM_NOMATCH;
            str++; continue;
        }
        // Literal match
        char pc = *pat, sc = *str;
        if (flags & FNM_CASEFOLD) {
            pc = (char)tolower((unsigned char)pc);
            sc = (char)tolower((unsigned char)sc);
        }
        if (pc != sc) return FNM_NOMATCH;
        pat++; str++;
    }
    return *str ? FNM_NOMATCH : 0;
}

int fnmatch(const char* pattern, const char* string, int flags) {
    return fnmatch_impl(pattern, string, flags, 0, string);
}

// ── POSIX regex (NFA-based ERE engine) ───────────────────────────────────
// A minimal but correct NFA regex engine supporting:
//   . ^ $ * + ? | () [] {n,m} \d \w \s etc.
// Stored as a bytecoded NFA; no external deps.

// NFA opcodes
#define ROP_CHAR    1   // match literal char
#define ROP_ANY     2   // match any char (.)
#define ROP_CLASS   3   // match char class [...]
#define ROP_NCLASS  4   // negated char class
#define ROP_BOL     5   // ^
#define ROP_EOL     6   // $
#define ROP_SPLIT   7   // fork to two continuations (a|b, ?, *, +)
#define ROP_JMP     8   // unconditional jump
#define ROP_SAVE    9   // save capture group start/end
#define ROP_MATCH  10   // success

#define REGEX_MAX_OPS   1024
#define REGEX_MAX_CLASS  256

typedef struct {
    uint8_t  op;
    union {
        char     ch;                      // ROP_CHAR
        uint8_t  cls[32];                // ROP_CLASS/NCLASS (256-bit bitmap)
        int      off;                     // ROP_JMP, ROP_SPLIT (second fork)
        int      save_idx;               // ROP_SAVE
    };
    int next;   // fall-through next index (-1 = none)
    int alt;    // alternate for ROP_SPLIT (-1 = none)
} re_op_t;

typedef struct {
    re_op_t  ops[REGEX_MAX_OPS];
    int      nops;
    int      nsub;
    int      flags;
} re_prog_t;

// Compile state
typedef struct {
    const char* src;
    int         pos;
    re_prog_t*  prog;
    int         flags;
    int         nsub;
} re_compile_t;

static int re_emit(re_compile_t* c, uint8_t op) {
    if (c->prog->nops >= REGEX_MAX_OPS) return -1;
    re_op_t* o = &c->prog->ops[c->prog->nops];
    o->op   = op;
    o->next = -1;
    o->alt  = -1;
    return c->prog->nops++;
}

// Forward declaration
static int re_parse_alt(re_compile_t* c, int* start, int* end);

static int re_parse_class(re_compile_t* c, uint8_t cls[32], int* negate) {
    *negate = 0;
    if (c->src[c->pos] == '^') { *negate = 1; c->pos++; }
    memset(cls, 0, 32);
    int first = 1;
    while (c->src[c->pos] && (c->src[c->pos] != ']' || first)) {
        first = 0;
        unsigned char lo, hi;
        if (c->src[c->pos] == '\\' && c->src[c->pos+1]) {
            c->pos++;
            unsigned char esc = (unsigned char)c->src[c->pos++];
            // Handle \d \w \s \D \W \S
            if (esc == 'd') {
                for (int i = '0'; i <= '9'; i++) cls[i/8] |= 1u << (i%8);
                continue;
            } else if (esc == 'D') {
                for (int i = 0; i < 256; i++) if (i < '0' || i > '9') cls[i/8] |= 1u << (i%8);
                continue;
            } else if (esc == 'w') {
                for (int i = 0; i < 256; i++) if (isalnum(i) || i == '_') cls[i/8] |= 1u << (i%8);
                continue;
            } else if (esc == 'W') {
                for (int i = 0; i < 256; i++) if (!isalnum(i) && i != '_') cls[i/8] |= 1u << (i%8);
                continue;
            } else if (esc == 's') {
                for (int i = 0; i < 256; i++) if (isspace(i)) cls[i/8] |= 1u << (i%8);
                continue;
            } else if (esc == 'S') {
                for (int i = 0; i < 256; i++) if (!isspace(i)) cls[i/8] |= 1u << (i%8);
                continue;
            }
            lo = hi = esc;
        } else {
            lo = (unsigned char)c->src[c->pos++];
            hi = lo;
        }
        if (c->src[c->pos] == '-' && c->src[c->pos+1] && c->src[c->pos+1] != ']') {
            c->pos++;  // skip '-'
            if (c->src[c->pos] == '\\' && c->src[c->pos+1]) { c->pos++; hi = (unsigned char)c->src[c->pos++]; }
            else hi = (unsigned char)c->src[c->pos++];
        }
        unsigned char a = lo, b = hi;
        if (c->flags & REG_ICASE) {
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
        }
        for (unsigned int i = a; i <= b; i++) cls[i/8] |= 1u << (i%8);
        if (c->flags & REG_ICASE) {
            for (unsigned int i = lo; i <= hi; i++) {
                unsigned int u = (i >= 'a' && i <= 'z') ? i - 32 : (i >= 'A' && i <= 'Z') ? i + 32 : 0;
                if (u) cls[u/8] |= 1u << (u%8);
            }
        }
    }
    if (c->src[c->pos] == ']') c->pos++;
    return 0;
}

static int re_parse_atom(re_compile_t* c, int* start, int* end) {
    char ch = c->src[c->pos];
    if (!ch || ch == ')' || ch == '|') return -1;
    if (ch == '(') {
        c->pos++;
        int grp = c->nsub++;
        int s0 = re_emit(c, ROP_SAVE); c->prog->ops[s0].save_idx = grp * 2;
        int astart, aend;
        if (re_parse_alt(c, &astart, &aend) < 0) return -1;
        if (c->src[c->pos] == ')') c->pos++;
        int s1 = re_emit(c, ROP_SAVE); c->prog->ops[s1].save_idx = grp * 2 + 1;
        // chain: s0 -> astart; aend -> s1
        c->prog->ops[s0].next = astart;
        if (aend >= 0) c->prog->ops[aend].next = s1;
        *start = s0; *end = s1;
        return 0;
    }
    if (ch == '[') {
        c->pos++;
        int idx = re_emit(c, ROP_CLASS);
        int neg; re_parse_class(c, c->prog->ops[idx].cls, &neg);
        if (neg) c->prog->ops[idx].op = ROP_NCLASS;
        *start = *end = idx;
        return 0;
    }
    if (ch == '.') {
        c->pos++;
        int idx = re_emit(c, ROP_ANY);
        *start = *end = idx;
        return 0;
    }
    if (ch == '^') {
        c->pos++;
        int idx = re_emit(c, ROP_BOL);
        *start = *end = idx;
        return 0;
    }
    if (ch == '$') {
        c->pos++;
        int idx = re_emit(c, ROP_EOL);
        *start = *end = idx;
        return 0;
    }
    if (ch == '\\' && c->src[c->pos+1]) {
        c->pos++;
        unsigned char esc = (unsigned char)c->src[c->pos++];
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
            // treat as a class
            int idx = re_emit(c, ROP_CLASS);
            uint8_t* cls = c->prog->ops[idx].cls; memset(cls, 0, 32);
            if (esc == 'd') for (int i='0';i<='9';i++) cls[i/8]|=1u<<(i%8);
            else if (esc=='D') for (int i=0;i<256;i++) { if(i<'0'||i>'9') cls[i/8]|=1u<<(i%8); }
            else if (esc=='w') for (int i=0;i<256;i++) { if(isalnum(i)||i=='_') cls[i/8]|=1u<<(i%8); }
            else if (esc=='W') for (int i=0;i<256;i++) { if(!isalnum(i)&&i!='_') cls[i/8]|=1u<<(i%8); }
            else if (esc=='s') for (int i=0;i<256;i++) { if(isspace(i)) cls[i/8]|=1u<<(i%8); }
            else               for (int i=0;i<256;i++) { if(!isspace(i)) cls[i/8]|=1u<<(i%8); }
            *start = *end = idx;
            return 0;
        }
        int idx = re_emit(c, ROP_CHAR);
        c->prog->ops[idx].ch = (char)esc;
        *start = *end = idx;
        return 0;
    }
    // Literal
    c->pos++;
    int idx = re_emit(c, ROP_CHAR);
    unsigned char lc = (unsigned char)ch;
    if (c->flags & REG_ICASE) lc = (unsigned char)tolower(lc);
    c->prog->ops[idx].ch = (char)lc;
    *start = *end = idx;
    return 0;
}

static int re_parse_piece(re_compile_t* c, int* start, int* end) {
    int astart = -1, aend = -1;
    if (re_parse_atom(c, &astart, &aend) < 0) return -1;
    char q = c->src[c->pos];
    if (q == '*' || q == '+' || q == '?' || q == '{') {
        c->pos++;
        // Skip {n,m} content for now (treat as *)
        if (q == '{') { while (c->src[c->pos] && c->src[c->pos] != '}') c->pos++; if (c->src[c->pos]) c->pos++; q = '*'; }
        int greedy = 1;
        if (c->src[c->pos] == '?') { greedy = 0; c->pos++; }
        int split = re_emit(c, ROP_SPLIT);
        if (q == '*') {
            // split -> atom, atom -> split; split also -> after
            c->prog->ops[split].next = greedy ? astart : -1;
            c->prog->ops[split].alt  = greedy ? -1 : astart;
            if (aend >= 0) c->prog->ops[aend].next = split;
            *start = split; *end = split;
        } else if (q == '+') {
            // atom -> split; split -> atom or exit
            if (aend >= 0) c->prog->ops[aend].next = split;
            c->prog->ops[split].next = greedy ? astart : -1;
            c->prog->ops[split].alt  = greedy ? -1 : astart;
            *start = astart; *end = split;
        } else { // ?
            c->prog->ops[split].next = greedy ? astart : -1;
            c->prog->ops[split].alt  = greedy ? -1 : astart;
            *start = split; *end = (aend >= 0 ? aend : split);
        }
    } else {
        *start = astart; *end = aend;
    }
    return 0;
}

static int re_parse_concat(re_compile_t* c, int* start, int* end) {
    int first_start = -1, prev_end = -1;
    while (c->src[c->pos] && c->src[c->pos] != ')' && c->src[c->pos] != '|') {
        int ps, pe;
        if (re_parse_piece(c, &ps, &pe) < 0) break;
        if (first_start < 0) first_start = ps;
        if (prev_end >= 0 && ps >= 0) c->prog->ops[prev_end].next = ps;
        prev_end = pe;
    }
    *start = first_start;
    *end   = prev_end;
    return 0;
}

static int re_parse_alt(re_compile_t* c, int* start, int* end) {
    int cs, ce;
    if (re_parse_concat(c, &cs, &ce) < 0) return -1;
    if (c->src[c->pos] != '|') { *start = cs; *end = ce; return 0; }
    // alternation: split -> left, split.alt -> right; both end at same merge point
    while (c->src[c->pos] == '|') {
        c->pos++;
        int split = re_emit(c, ROP_SPLIT);
        int rs, re2;
        re_parse_concat(c, &rs, &re2);
        c->prog->ops[split].next = cs;
        c->prog->ops[split].alt  = rs;
        // The ends of both branches are left dangling; caller chains them.
        cs = split; (void)ce; (void)re2;
        ce = -1;  // merged end unknown at this level — handled by parent
    }
    *start = cs; *end = ce;
    return 0;
}

int regcomp(regex_t* preg, const char* pattern, int cflags) {
    re_prog_t* prog = malloc(sizeof(re_prog_t));
    if (!prog) return REG_ESPACE;
    prog->nops  = 0;
    prog->nsub  = 0;
    prog->flags = cflags;

    re_compile_t c;
    c.src   = pattern;
    c.pos   = 0;
    c.prog  = prog;
    c.flags = cflags;
    c.nsub  = 1;  // group 0 = whole match

    int s, e;
    // Wrap whole pattern in implicit group 0 save
    int s0 = re_emit(&c, ROP_SAVE); prog->ops[s0].save_idx = 0;
    if (re_parse_alt(&c, &s, &e) < 0) { free(prog); return REG_BADPAT; }
    if (s >= 0) prog->ops[s0].next = s;
    int s1 = re_emit(&c, ROP_SAVE); prog->ops[s1].save_idx = 1;
    if (e >= 0) prog->ops[e].next = s1;
    else if (s >= 0) prog->ops[s].next = s1;
    int match = re_emit(&c, ROP_MATCH);
    prog->ops[s1].next = match;
    prog->nsub = c.nsub;

    preg->re_nsub  = c.nsub - 1;
    preg->_internal = prog;
    return 0;
}

// NFA simulation via recursive backtracking with memoization-lite
#define RE_MAX_CAPTURES 64

typedef struct {
    const char* start;    // string start (for ^ checking)
    const char* str;      // current position
    int         flags;
    regmatch_t* pm;
    size_t      nmatch;
} re_exec_t;

static int re_run(re_exec_t* ex, re_prog_t* prog, int pc, const char* sp);

static int re_match_char(re_exec_t* ex, re_op_t* op, const char* sp) {
    if (!*sp) return 0;
    unsigned char sc = (unsigned char)*sp;
    if (ex->flags & REG_ICASE) sc = (unsigned char)tolower(sc);
    if (op->op == ROP_CHAR) {
        return (sc == (unsigned char)op->ch);
    } else if (op->op == ROP_ANY) {
        return (ex->flags & REG_NEWLINE) ? (*sp != '\n') : 1;
    } else if (op->op == ROP_CLASS) {
        return (op->cls[sc/8] >> (sc%8)) & 1;
    } else {  // NCLASS
        return !((op->cls[sc/8] >> (sc%8)) & 1);
    }
}

static int re_run(re_exec_t* ex, re_prog_t* prog, int pc, const char* sp) {
    while (pc >= 0 && pc < prog->nops) {
        re_op_t* op = &prog->ops[pc];
        switch (op->op) {
        case ROP_MATCH:
            return 1;
        case ROP_BOL:
            if (sp != ex->start && !(ex->flags & REG_NOTBOL)) return 0;
            pc = op->next; break;
        case ROP_EOL:
            if (*sp && !((ex->flags & REG_NEWLINE) && *sp == '\n')) return 0;
            pc = op->next; break;
        case ROP_SAVE:
            if (op->save_idx < (int)(ex->nmatch * 2)) {
                const char* saved = (op->save_idx & 1)
                    ? (const char*)ex->pm[op->save_idx/2].rm_eo + (size_t)ex->str
                    : (const char*)ex->pm[op->save_idx/2].rm_so + (size_t)ex->str;
                // save & try next, restore on failure
                size_t old;
                if (op->save_idx & 1) { old = (size_t)ex->pm[op->save_idx/2].rm_eo; ex->pm[op->save_idx/2].rm_eo = sp - ex->str; }
                else                  { old = (size_t)ex->pm[op->save_idx/2].rm_so; ex->pm[op->save_idx/2].rm_so = sp - ex->str; }
                (void)saved;
                if (re_run(ex, prog, op->next, sp)) return 1;
                if (op->save_idx & 1) ex->pm[op->save_idx/2].rm_eo = (ssize_t)old;
                else                  ex->pm[op->save_idx/2].rm_so = (ssize_t)old;
                return 0;
            }
            pc = op->next; break;
        case ROP_SPLIT:
            if (re_run(ex, prog, op->next, sp)) return 1;
            pc = op->alt; break;
        case ROP_JMP:
            pc = op->next; break;
        case ROP_CHAR: case ROP_ANY: case ROP_CLASS: case ROP_NCLASS:
            if (!re_match_char(ex, op, sp)) return 0;
            sp++;
            pc = op->next;
            break;
        default: return 0;
        }
    }
    return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    re_prog_t* prog = (re_prog_t*)preg->_internal;
    if (!prog || !string) return REG_NOMATCH;

    // Initialize matches to -1
    for (size_t i = 0; i < nmatch; i++) { pmatch[i].rm_so = -1; pmatch[i].rm_eo = -1; }

    re_exec_t ex;
    ex.start  = string;
    ex.flags  = prog->flags | eflags;
    ex.pm     = pmatch;
    ex.nmatch = nmatch;

    // Try matching at each position
    const char* sp = string;
    do {
        ex.str = sp;
        if (re_run(&ex, prog, 0, sp)) return 0;
        sp++;
    } while (*(sp-1));
    return REG_NOMATCH;
}

void regfree(regex_t* preg) {
    if (preg && preg->_internal) { free(preg->_internal); preg->_internal = NULL; }
}

size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size) {
    (void)preg;
    const char* msg;
    switch (errcode) {
    case 0:          msg = "Success"; break;
    case REG_NOMATCH:msg = "No match"; break;
    case REG_BADPAT: msg = "Invalid regular expression"; break;
    case REG_ESPACE: msg = "Out of memory"; break;
    default:         msg = "Unknown regex error"; break;
    }
    size_t n = strnlen(msg, 255);
    if (errbuf && errbuf_size) {
        size_t cp = n < errbuf_size - 1 ? n : errbuf_size - 1;
        memcpy(errbuf, msg, cp); errbuf[cp] = '\0';
    }
    return n + 1;
}

// ── __libc_start_main ─────────────────────────────────────────────────────
// Bridges glibc CRT calling convention to our libc.
// argv[0..argc-1] and envp are passed by our ELF loader.
int __libc_start_main(int (*main)(int, char**, char**),
                      int argc, char** argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void* stack_end) {
    (void)fini; (void)rtld_fini; (void)stack_end;
    // envp follows argv[] + NULL on the stack
    char** envp = argv + argc + 1;
    environ = envp;
    if (init) init();
    int ret = main(argc, argv, envp);
    exit(ret);
}

// ── sigjmp_buf / __sigsetjmp / siglongjmp ────────────────────────────────
// We reuse our setjmp/longjmp from setjmp.asm, extended with mask save.
extern int  setjmp(long long* env);
extern void longjmp(long long* env, int val) __attribute__((noreturn));

int __sigsetjmp(sigjmp_buf env, int savesigs) {
    env[0]._savesigs = savesigs;
    if (savesigs) sigprocmask(SIG_BLOCK, NULL, &env[0]._mask);
    return setjmp(env[0]._regs);
}

__attribute__((noreturn)) void siglongjmp(sigjmp_buf env, int val) {
    if (env[0]._savesigs) sigprocmask(SIG_SETMASK, &env[0]._mask, NULL);
    longjmp(env[0]._regs, val);
}

// ── termcap ──────────────────────────────────────────────────────────────
// Real termcap implementation for VT100/ANSI/linux terminals.
// Uses a built-in capability table rather than reading /etc/termcap.
// All escape sequences follow the ANSI X3.64 / ECMA-48 standard.

char  PC = '\0';
char* BC = NULL;
char* UP = NULL;

// Capability database — strings stored in a static arena.
// tgetstr copies into *area if non-NULL, else returns from the arena.
static char s_tc_arena[1024];
static int  s_tc_arena_pos = 0;

static char* tc_store(const char* s) {
    int len = 0; while (s[len]) len++;
    if (s_tc_arena_pos + len + 1 > (int)sizeof(s_tc_arena))
        return NULL;
    char* p = &s_tc_arena[s_tc_arena_pos];
    for (int i = 0; i <= len; i++) p[i] = s[i];
    s_tc_arena_pos += len + 1;
    return p;
}

// Terminal size (queried once in tgetent, used by tgetnum)
static int s_tc_lines = 25;
static int s_tc_cols  = 80;

int tgetent(char* bp, const char* name) {
    (void)bp; (void)name;
    // Query actual terminal size via TIOCGWINSZ if available
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        s_tc_lines = ws.ws_row;
        s_tc_cols  = ws.ws_col;
    }
    // Set global termcap variables
    BC = tc_store("\b");
    UP = tc_store("\033[A");
    return 1; // success
}

int tgetflag(const char* id) {
    if (!id) return 0;
    // am: auto-margins (terminal wraps at right edge)
    if (id[0] == 'a' && id[1] == 'm') return 1;
    // bs: can backspace with ^H
    if (id[0] == 'b' && id[1] == 's') return 1;
    return 0;
}

int tgetnum(const char* id) {
    if (!id) return -1;
    // li: lines on screen
    if (id[0] == 'l' && id[1] == 'i') return s_tc_lines;
    // co: columns on screen
    if (id[0] == 'c' && id[1] == 'o') return s_tc_cols;
    // sg: standout glitch width (0 = no glitch)
    if (id[0] == 's' && id[1] == 'g') return 0;
    return -1;
}

// String capability lookup table — only capabilities the raw framebuffer
// terminal actually supports (no ANSI escape sequences).
// Readline falls back to full-line redraws when cursor-motion/insert/delete
// capabilities are absent, which works correctly on the dumb fb terminal.
static const struct { char id[3]; const char* seq; } s_tc_strings[] = {
    {"le", "\b"},              // cursor left
    {"cr", "\r"},              // carriage return
    {"nw", "\r\n"},            // newline
    {"sf", "\n"},              // scroll forward
    {"bl", "\007"},            // audible bell
    {"bc", "\b"},              // backspace character
    {"pc", ""},                // pad character
    {"im", ""},                // enter insert mode (no-op)
    {"ei", ""},                // exit insert mode (no-op)

    {"", NULL}, // sentinel
};

char* tgetstr(const char* id, char** area) {
    if (!id) return NULL;
    for (int i = 0; s_tc_strings[i].seq; i++) {
        if (s_tc_strings[i].id[0] == id[0] && s_tc_strings[i].id[1] == id[1]) {
            const char* seq = s_tc_strings[i].seq;
            if (area && *area) {
                // Copy into caller's buffer and advance pointer
                char* dst = *area;
                int j = 0;
                while (seq[j]) { dst[j] = seq[j]; j++; }
                dst[j] = '\0';
                *area = dst + j + 1;
                return dst;
            }
            // Return from static arena
            return tc_store(seq);
        }
    }
    return NULL;
}

// tgoto: substitute parameters into a cursor-addressing string.
// Handles the %d, %i, %+, %., %> format codes from termcap.
static char s_tgoto_buf[64];

char* tgoto(const char* cap, int col, int row) {
    if (!cap) return NULL;
    char* out = s_tgoto_buf;
    int pos = 0;
    int args[2] = {row, col};
    int argidx = 0;
    int one_based = 0;

    while (*cap && pos < 60) {
        if (*cap == '%') {
            cap++;
            switch (*cap) {
            case 'd': {
                int v = args[argidx++] + one_based;
                if (v >= 100) out[pos++] = '0' + v / 100;
                if (v >= 10)  out[pos++] = '0' + (v / 10) % 10;
                out[pos++] = '0' + v % 10;
                cap++;
                break;
            }
            case 'i':
                one_based = 1;
                cap++;
                break;
            case '+':
                cap++;
                out[pos++] = (char)(args[argidx++] + *cap);
                cap++;
                break;
            case '.':
                out[pos++] = (char)args[argidx++];
                cap++;
                break;
            case '%':
                out[pos++] = '%';
                cap++;
                break;
            default:
                out[pos++] = '%';
                out[pos++] = *cap++;
                break;
            }
        } else {
            out[pos++] = *cap++;
        }
    }
    out[pos] = '\0';
    return s_tgoto_buf;
}

int tputs(const char* str, int affcnt, int (*putc_fn)(int)) {
    (void)affcnt;
    if (!str) return 0;
    // Skip leading padding specification (digits and optional '.' and '*')
    while ((*str >= '0' && *str <= '9') || *str == '.' || *str == '*')
        str++;
    while (*str)
        putc_fn((unsigned char)*str++);
    return 0;
}



// ── Forward declarations for the new libinput-port libc surface.
// libc.h doesn't pull in <sys/utsname.h> / <stdio.h> directly, and
// dragging the sysroot copies in here triggers a type conflict on
// clock_t / ssize_t.  Minimal local forward-decls suffice.
FILE* fdopen(int fd, const char* mode);
int   unlink(const char* path);

// ── atof ─────────────────────────────────────────────────────────────────
// Thin wrapper around strtod: discard the end-pointer.
double atof(const char* s) { return strtod(s, 0); }


// ── tmpfile / mkstemp fallback ───────────────────────────────────────────
// Opens /tmp/mkosXXXXXX, O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC.  The file is
// unlinked before fdopen so it self-cleans on close.  Uses srand-seeded
// 6-char suffix — good enough for libinput-record tempfiles.
FILE* tmpfile(void) {
    char path[32];
    for (int tries = 0; tries < 64; tries++) {
        unsigned r = (unsigned)rand();
        snprintf(path, sizeof(path), "/tmp/mko%06u", r % 1000000u);
        int fd = open(path, O_CREAT|O_EXCL|O_RDWR, 0600);
        if (fd < 0) continue;
        unlink(path);
        FILE* fp = fdopen(fd, "w+");
        if (!fp) { close(fd); return 0; }
        return fp;
    }
    return 0;
}

// ── scanf family — minimal %d/%s/%u/%x/%lx/%ld — satisfies libinput. ─────
// libinput-record parses trivially-formatted event dumps with scanf.
// We don't need fancy conversion; only the primitives below.
static int scanf_core(const char** bufp, const char* fmt, va_list ap);
int vsscanf(const char* buf, const char* fmt, va_list ap) {
    const char* b = buf;
    return scanf_core(&b, fmt, ap);
}
static int scanf_core(const char** bufp, const char* fmt, va_list ap) {
    int matched = 0;
    const char* b = *bufp;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*b == ' ' || *b == '\t' || *b == '\n') b++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*b != *fmt) break;
            b++; fmt++;
            continue;
        }
        fmt++;
        int longflag = 0, llflag = 0;
        while (*fmt == 'l') { longflag++; if (longflag == 2) llflag = 1; fmt++; }
        while (*b == ' ' || *b == '\t' || *b == '\n') b++;
        switch (*fmt) {
        case 'd': {
            int neg = 0;
            if (*b == '-') { neg = 1; b++; } else if (*b == '+') b++;
            if (*b < '0' || *b > '9') return matched;
            long long v = 0;
            while (*b >= '0' && *b <= '9') { v = v*10 + (*b - '0'); b++; }
            if (neg) v = -v;
            if (llflag)      *va_arg(ap, long long*) = v;
            else if (longflag) *va_arg(ap, long*)    = (long)v;
            else               *va_arg(ap, int*)     = (int)v;
            matched++;
            break;
        }
        case 'u': case 'x': {
            int base = (*fmt == 'x') ? 16 : 10;
            if (base == 16 && b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) b += 2;
            unsigned long long v = 0; int consumed = 0;
            while (*b) {
                int d;
                if (*b >= '0' && *b <= '9') d = *b - '0';
                else if (base == 16 && *b >= 'a' && *b <= 'f') d = *b - 'a' + 10;
                else if (base == 16 && *b >= 'A' && *b <= 'F') d = *b - 'A' + 10;
                else break;
                v = v * base + d; b++; consumed++;
            }
            if (!consumed) return matched;
            if (llflag)      *va_arg(ap, unsigned long long*) = v;
            else if (longflag) *va_arg(ap, unsigned long*)    = (unsigned long)v;
            else               *va_arg(ap, unsigned int*)     = (unsigned int)v;
            matched++;
            break;
        }
        case 's': {
            char* out = va_arg(ap, char*);
            int i = 0;
            while (*b && *b != ' ' && *b != '\t' && *b != '\n') { out[i++] = *b++; }
            out[i] = 0;
            if (!i) return matched;
            matched++;
            break;
        }
        case 'c': {
            char* out = va_arg(ap, char*);
            if (!*b) return matched;
            *out = *b++;
            matched++;
            break;
        }
        case '%': if (*b++ != '%') return matched; break;
        default: return matched;
        }
        fmt++;
    }
    *bufp = b;
    return matched;
}

// ── setvbuf ───────────────────────────────────────────────────────────────
// Defined in stdio.c but needs to be reachable from libc.c's declarations.
// Forward-declare; actual body is in stdio.c.

// SDL3 / port-surface extern impls — moved to sdl_port_stubs.c,
// which is compiled with SYSROOT_CFLAGS so its sysroot-header
// types (wchar_t, size_t, …) agree with the ports that consume
// them.  Compiling them here against in-tree libc.h caused a
// wchar.h redefinition storm.
