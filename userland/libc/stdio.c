#include "libc.h"
#include "stdio.h"

// ── va_list macros ────────────────────────────────────────────────────────
#ifndef _VA_LIST_DEFINED
#define _VA_LIST_DEFINED
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
#endif

// ── vsnprintf_impl forward declaration ───────────────────────────────────
// Implemented in libc.c; we call it for fprintf/vfprintf.
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

// ── Static FILE storage for stdin/stdout/stderr ───────────────────────────

static FILE s_stdin_file  = { .fd = 0, .flags = _FILE_READ,  .rpos=0, .rlen=0, .wpos=0, .file_pos=0 };
static FILE s_stdout_file = { .fd = 1, .flags = _FILE_WRITE, .rpos=0, .rlen=0, .wpos=0, .file_pos=0 };
static FILE s_stderr_file = { .fd = 2, .flags = _FILE_WRITE | _FILE_UNBUF, .rpos=0, .rlen=0, .wpos=0, .file_pos=0 };

FILE* stdin  = &s_stdin_file;
FILE* stdout = &s_stdout_file;
FILE* stderr = &s_stderr_file;

// ── Pool of FILE objects ───────────────────────────────────────────────────
// Doom opens at most a handful of files simultaneously.
#define MAX_FILES 32

static FILE s_file_pool[MAX_FILES];
static uint8_t s_file_used[MAX_FILES];  // 1 = in use

static FILE* alloc_file(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!s_file_used[i]) {
            s_file_used[i] = 1;
            memset(&s_file_pool[i], 0, sizeof(FILE));
            return &s_file_pool[i];
        }
    }
    return NULL;  // out of FILE slots
}

static void free_file(FILE* f) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (&s_file_pool[i] == f) {
            s_file_used[i] = 0;
            return;
        }
    }
    // stdin/stdout/stderr are not in the pool — that's fine.
}

// ── Internal flush ────────────────────────────────────────────────────────

static int flush_write(FILE* f) {
    if (!f->wpos) return 0;
    ssize_t n = write(f->fd, f->wbuf, (size_t)f->wpos);
    if (n < 0) { f->flags |= _FILE_ERR; return EOF; }
    f->file_pos += n;
    f->wpos = 0;
    return 0;
}

// ── Internal fill read buffer ─────────────────────────────────────────────

static int fill_read(FILE* f) {
    ssize_t n = read(f->fd, f->rbuf, STDIO_BUFSIZ);
    if (n < 0) { f->flags |= _FILE_ERR; f->rpos = f->rlen = 0; return EOF; }
    if (n == 0) { f->flags |= _FILE_EOF; f->rpos = f->rlen = 0; return EOF; }
    f->rpos = 0;
    f->rlen = (int)n;
    return 0;
}

// ── fopen ─────────────────────────────────────────────────────────────────

FILE* fopen(const char* path, const char* mode) {
    int flags = 0;
    int file_flags = 0;

    // Parse mode string: "r", "w", "a", "r+", "w+", "a+", with optional 'b'
    int reading = 0, writing = 0, appending = 0, updating = 0;
    for (const char* m = mode; *m; m++) {
        if (*m == 'r') reading  = 1;
        if (*m == 'w') writing  = 1;
        if (*m == 'a') appending = 1;
        if (*m == '+') updating  = 1;
        // 'b' is silently accepted (binary mode — same as text in our FS)
    }

    if (reading && !updating) {
        flags = O_RDONLY;
        file_flags = _FILE_READ;
    } else if (writing && !updating) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        file_flags = _FILE_WRITE;
    } else if (appending && !updating) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        file_flags = _FILE_WRITE;
    } else if (reading && updating) {
        flags = O_RDWR;
        file_flags = _FILE_READ | _FILE_WRITE;
    } else if (writing && updating) {
        flags = O_RDWR | O_CREAT | O_TRUNC;
        file_flags = _FILE_READ | _FILE_WRITE;
    } else if (appending && updating) {
        flags = O_RDWR | O_CREAT | O_APPEND;
        file_flags = _FILE_READ | _FILE_WRITE;
    } else {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;

    FILE* f = alloc_file();
    if (!f) { close(fd); errno = ENOMEM; return NULL; }

    f->fd       = fd;
    f->flags    = file_flags;
    f->rpos     = 0;
    f->rlen     = 0;
    f->wpos     = 0;
    f->file_pos = 0;
    return f;
}

FILE* fdopen(int fd, const char* mode) {
    int file_flags = 0;
    for (const char* m = mode; *m; m++) {
        if (*m == 'r') file_flags |= _FILE_READ;
        if (*m == 'w') file_flags |= _FILE_WRITE;
        if (*m == 'a') file_flags |= _FILE_WRITE;
        if (*m == '+') file_flags |= _FILE_READ | _FILE_WRITE;
    }
    FILE* f = alloc_file();
    if (!f) { errno = ENOMEM; return NULL; }
    f->fd       = fd;
    f->flags    = file_flags;
    f->rpos     = 0;
    f->rlen     = 0;
    f->wpos     = 0;
    f->file_pos = 0;
    return f;
}

// ── fclose ────────────────────────────────────────────────────────────────

int fclose(FILE* f) {
    if (!f || f->fd < 0) return EOF;
    int err = 0;
    if (f->flags & _FILE_WRITE) err = flush_write(f);
    close(f->fd);
    f->fd = -1;
    free_file(f);
    return err;
}

// ── _flush_all — called by exit() before SYS_EXIT ────────────────────────

void _flush_all(void) {
    flush_write(stdout);
    flush_write(stderr);
}

// ── fflush ────────────────────────────────────────────────────────────────

int fflush(FILE* f) {
    if (!f) {
        // fflush(NULL): flush all open write streams.
        // We don't track them all; just flush stdout/stderr.
        flush_write(stdout);
        flush_write(stderr);
        return 0;
    }
    if (f->flags & _FILE_WRITE) return flush_write(f);
    return 0;
}

// ── fgetc ─────────────────────────────────────────────────────────────────

int fgetc(FILE* f) {
    if (!(f->flags & _FILE_READ)) { f->flags |= _FILE_ERR; return EOF; }
    if (f->flags & (_FILE_ERR | _FILE_EOF)) return EOF;
    if (f->rpos >= f->rlen) {
        if (fill_read(f) == EOF) return EOF;
    }
    int c = (unsigned char)f->rbuf[f->rpos++];
    f->file_pos++;
    return c;
}

int getchar(void) {
    return fgetc(stdin);
}

// ── fputc ─────────────────────────────────────────────────────────────────

int fputc(int c, FILE* f) {
    if (!(f->flags & _FILE_WRITE)) { f->flags |= _FILE_ERR; return EOF; }
    if (f->flags & _FILE_ERR) return EOF;
    f->wbuf[f->wpos++] = (uint8_t)c;
    if (f->wpos >= STDIO_BUFSIZ || c == '\n' || (f->flags & _FILE_UNBUF)) {
        if (flush_write(f) == EOF) return EOF;
    }
    return (unsigned char)c;
}

// ── fgets ─────────────────────────────────────────────────────────────────

char* fgets(char* s, int n, FILE* f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

// ── fputs ─────────────────────────────────────────────────────────────────

int fputs(const char* s, FILE* f) {
    while (*s) { if (fputc((unsigned char)*s, f) == EOF) return EOF; s++; }
    return 0;
}

// ── fread ─────────────────────────────────────────────────────────────────

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!size || !nmemb) return 0;
    uint8_t* dst = (uint8_t*)ptr;
    size_t total = size * nmemb;
    size_t done  = 0;

    while (done < total) {
        if (f->flags & (_FILE_ERR | _FILE_EOF)) break;

        // Drain the read buffer first.
        if (f->rpos < f->rlen) {
            size_t avail = (size_t)(f->rlen - f->rpos);
            size_t want  = total - done;
            size_t take  = avail < want ? avail : want;
            memcpy(dst + done, f->rbuf + f->rpos, take);
            f->rpos     += (int)take;
            f->file_pos += (long)take;
            done        += take;
            continue;
        }

        // Buffer empty. For large reads bypass the buffer.
        size_t remaining = total - done;
        if (remaining >= STDIO_BUFSIZ) {
            ssize_t n = read(f->fd, dst + done, remaining);
            if (n < 0) { f->flags |= _FILE_ERR; break; }
            if (n == 0) { f->flags |= _FILE_EOF; break; }
            f->file_pos += (long)n;
            done += (size_t)n;
        } else {
            if (fill_read(f) == EOF) break;
        }
    }

    return done / size;
}

// ── fwrite ────────────────────────────────────────────────────────────────

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!size || !nmemb) return 0;
    const uint8_t* src = (const uint8_t*)ptr;
    size_t total = size * nmemb;
    size_t done  = 0;

    while (done < total) {
        if (f->flags & _FILE_ERR) break;

        size_t space = (size_t)(STDIO_BUFSIZ - f->wpos);
        size_t want  = total - done;
        size_t put   = space < want ? space : want;
        memcpy(f->wbuf + f->wpos, src + done, put);
        f->wpos += (int)put;
        done    += put;

        if (f->wpos >= STDIO_BUFSIZ || (f->flags & _FILE_UNBUF)) {
            if (flush_write(f) == EOF) break;
        }
    }

    return done / size;
}

// ── fseek ─────────────────────────────────────────────────────────────────

int fseek(FILE* f, long offset, int whence) {
    // Flush write buffer first.
    if ((f->flags & _FILE_WRITE) && f->wpos) flush_write(f);
    // Invalidate read buffer.
    f->rpos = f->rlen = 0;

    long new_pos = lseek(f->fd, offset, whence);
    if (new_pos < 0) return -1;
    f->file_pos = new_pos;
    f->flags &= ~(_FILE_ERR | _FILE_EOF);
    return 0;
}

// ── ftell ─────────────────────────────────────────────────────────────────

long ftell(FILE* f) {
    // Account for buffered but not-yet-read data.
    return f->file_pos - (long)(f->rlen - f->rpos);
}

// ── rewind ────────────────────────────────────────────────────────────────

void rewind(FILE* f) {
    fseek(f, 0, SEEK_SET);
}

// ── feof / ferror / clearerr ──────────────────────────────────────────────

int feof(FILE* f)   { return !!(f->flags & _FILE_EOF); }
int ferror(FILE* f) { return !!(f->flags & _FILE_ERR); }
void clearerr(FILE* f) { f->flags &= ~(_FILE_ERR | _FILE_EOF); }

// ── fprintf / vfprintf ────────────────────────────────────────────────────
// Use a stack buffer; for outputs larger than STDIO_BUFSIZ we fall back to
// fwrite of a heap-allocated buffer.

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) return n;
    if ((size_t)n < sizeof(buf)) {
        fwrite(buf, 1, (size_t)n, f);
        return n;
    }
    // Output was truncated — allocate a bigger buffer.
    char* big = (char*)malloc((size_t)(n + 1));
    if (!big) { fwrite(buf, 1, sizeof(buf) - 1, f); return -1; }
    vsnprintf(big, (size_t)(n + 1), fmt, ap);
    fwrite(big, 1, (size_t)n, f);
    free(big);
    return n;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char* fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
    (void)f; (void)buf; (void)mode; (void)size;
    return 0;  /* no buffering control — all writes go through immediately */
}

