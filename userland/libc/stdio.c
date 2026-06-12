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
static FILE s_stdout_file = { .fd = 1, .flags = _FILE_WRITE | _FILE_LNBUF, .rpos=0, .rlen=0, .wpos=0, .file_pos=0 };
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
    // Atomic claim — two threads fopen()ing concurrently (fcft's
    // worker pool scanning fonts) must never share a slot.
    for (int i = 0; i < MAX_FILES; i++) {
        uint8_t expect = 0;
        if (__atomic_compare_exchange_n(&s_file_used[i], &expect, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            memset(&s_file_pool[i], 0, sizeof(FILE));
            return &s_file_pool[i];
        }
    }
    return NULL;  // out of FILE slots
}

static void free_file(FILE* f) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (&s_file_pool[i] == f) {
            __atomic_store_n(&s_file_used[i], 0, __ATOMIC_RELEASE);
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

// freopen — re-target an existing stream (glib's gstdio).  Standard
// use is rebinding stdin/stdout/stderr, whose FILE storage is static,
// so the stream object must be reused rather than reallocated.
FILE* freopen(const char* path, const char* mode, FILE* f) {
    if (!f) return NULL;
    FILE* fresh = fopen(path, mode);
    if (!fresh) return NULL;
    if (f->flags & _FILE_WRITE) flush_write(f);
    if (f->fd >= 0) close(f->fd);
    int keep_flags = f->flags & (_FILE_UNBUF | _FILE_LNBUF);
    *f = *fresh;
    f->flags |= keep_flags;
    fresh->fd = -1;          // neutralize before releasing the pool slot
    free_file(fresh);
    return f;
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

// Parenthesised to dodge the getc() convenience macro in stdio.h —
// sysroot consumers link the real symbol.
int (getc)(FILE* f) {
    return fgetc(f);
}

// ungetc — push one character back into the read buffer.  The common
// caller pattern is getc-then-ungetc (glib's charset.alias parser,
// xdgmime magic sniffing), where rpos > 0 and the pushback slot is
// simply the previous buffer position.  The cold rpos==0 case shifts
// the buffered remainder right by one when there is room.
int ungetc(int c, FILE* f) {
    if (c == EOF || !f || !(f->flags & _FILE_READ)) return EOF;
    if (f->rpos > 0) {
        f->rbuf[--f->rpos] = (uint8_t)c;
    } else {
        if (f->rlen >= STDIO_BUFSIZ) return EOF;
        memmove(f->rbuf + 1, f->rbuf, (size_t)f->rlen);
        f->rbuf[0] = (uint8_t)c;
        f->rlen++;
    }
    f->flags &= ~_FILE_EOF;
    if (f->file_pos) f->file_pos--;
    return (uint8_t)c;
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

        int need_flush = (f->wpos >= STDIO_BUFSIZ) || (f->flags & _FILE_UNBUF);

        // Line-buffered: flush if the chunk we just wrote contains '\n'.
        if (!need_flush && (f->flags & _FILE_LNBUF)) {
            const uint8_t* chunk = src + done - put;
            for (size_t i = 0; i < put; i++) {
                if (chunk[i] == '\n') { need_flush = 1; break; }
            }
        }

        if (need_flush) {
            if (flush_write(f) == EOF) break;
        }
    }

    return done / size;
}

// ── fseek ─────────────────────────────────────────────────────────────────

int fseek(FILE* f, long offset, int whence) {
    // Flush write buffer first.
    if ((f->flags & _FILE_WRITE) && f->wpos) flush_write(f);

    // Resolve target absolute position so we can check whether it falls
    // inside the currently buffered range.  SEEK_CUR is relative to the
    // caller's view (file_pos already accounts for bytes drained from
    // rbuf), SEEK_END needs an explicit lseek probe.
    long target;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = f->file_pos + offset;
    } else {
        long end = lseek(f->fd, 0, SEEK_END);
        if (end < 0) return -1;
        target = end + offset;
        // The probe moved the KERNEL offset to EOF.  If the target
        // resolves into the in-memory buffer (fast path below), the
        // kernel offset must be restored to the buffer's fill point or
        // the next fill_read continues from EOF and the stream ends
        // early.  Restoring unconditionally is harmless for the slow
        // path, which re-seeks anyway.
        if (f->rlen > 0) {
            long buf_fill_end = (f->file_pos - f->rpos) + f->rlen;
            if (lseek(f->fd, buf_fill_end, SEEK_SET) < 0) return -1;
        }
    }
    if (target < 0) return -1;

    // Fast path: target lies inside the already-read buffer.  The buffer
    // spans file offsets [file_pos - rpos .. file_pos + (rlen - rpos)),
    // so just shift rpos without touching the kernel.  This is the
    // difference between freetype reading 343 KB once versus hammering
    // the disk for hundreds of MB of redundant 4 KB reads while doing
    // 2-byte fseek+fread probes over a TTF glyph table.
    if (f->rlen > 0) {
        long buf_base = f->file_pos - f->rpos;
        long buf_end  = buf_base + f->rlen;
        if (target >= buf_base && target <= buf_end) {
            f->rpos     = (int)(target - buf_base);
            f->file_pos = target;
            f->flags   &= ~(_FILE_ERR | _FILE_EOF);
            return 0;
        }
    }

    // Slow path: seek target outside buffered range.  Drop buffer and
    // re-sync the kernel offset.
    f->rpos = f->rlen = 0;
    long new_pos = lseek(f->fd, target, SEEK_SET);
    if (new_pos < 0) return -1;
    f->file_pos = new_pos;
    f->flags &= ~(_FILE_ERR | _FILE_EOF);
    return 0;
}

// ── ftell ─────────────────────────────────────────────────────────────────

long ftell(FILE* f) {
    // file_pos is maintained in CONSUMER units: fgetc/ungetc move it
    // per byte handed to (or taken back from) the caller, and
    // fill_read never touches it.  It already IS the logical stream
    // position.  The old code subtracted the unread buffer remainder
    // — a kernel-offset model this FILE doesn't use — which made
    // every ftell-snapshot/fseek-restore pair (sway's detect_brace
    // peeks a line ahead after each config command) rewind into
    // already-consumed bytes and replay the stream mid-line.
    return f->file_pos;
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

// POSIX dprintf/vdprintf — formatted output straight to a file descriptor
// (no FILE buffering).  Used by swaybar's i3bar status protocol and other
// ports.  Writes the formatted bytes with a single write(2) when they fit
// in the stack buffer, else via a heap bounce.
int vdprintf(int fd, const char* fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) return n;
    if ((size_t)n < sizeof(buf)) {
        return (int)write(fd, buf, (size_t)n);
    }
    char* big = (char*)malloc((size_t)(n + 1));
    if (!big) { write(fd, buf, sizeof(buf) - 1); return -1; }
    vsnprintf(big, (size_t)(n + 1), fmt, ap);
    int w = (int)write(fd, big, (size_t)n);
    free(big);
    return w;
}

int dprintf(int fd, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
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

// setbuf — thin wrapper over setvbuf.  POSIX says buf==NULL → _IONBF,
// otherwise → fully-buffered with the caller's buffer.
void setbuf(FILE* f, char* buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
    (void)buf; (void)size;
    if (!f) return -1;
    // Clear existing buffering flags, then set the requested mode.
    f->flags &= ~(_FILE_UNBUF | _FILE_LNBUF);
    if (mode == _IONBF)      f->flags |= _FILE_UNBUF;
    else if (mode == _IOLBF) f->flags |= _FILE_LNBUF;
    // _IOFBF (fully buffered) = default, no flag needed.
    return 0;
}

// ── perror — print a message followed by strerror(errno) to stderr ────────
// fontconfig uses it on cache-open failures.  Keep compact — we don't
// ship a strerror table yet, so just format "prefix: errno=N\n".
void perror(const char* prefix) {
    char buf[64];
    int n = 0;
    if (prefix) {
        while (prefix[n] && n < 40) { buf[n] = prefix[n]; n++; }
        buf[n++] = ':'; buf[n++] = ' ';
    }
    // Very small int → decimal for errno value.  Handles 0..999.
    int e = errno;
    if (e < 0) { buf[n++] = '-'; e = -e; }
    char digs[12]; int d = 0;
    do { digs[d++] = (char)('0' + (e % 10)); e /= 10; } while (e && d < 11);
    while (d) buf[n++] = digs[--d];
    buf[n++] = '\n';
    write(2, buf, (size_t)n);
}

// ── fileno ────────────────────────────────────────────────────────────────
// Extract underlying fd from FILE*.  libxkbcommon uses it for
// mmap-the-file-by-fd; libdrm uses it for select() on device fds.
int fileno(FILE* f) {
    if (!f) { errno = EBADF; return -1; }
    return f->fd;
}

// ── getline ───────────────────────────────────────────────────────────────
// POSIX.  Used by libdrm for uevent parsing.  Grows *lineptr as needed.
ssize_t getline(char** lineptr, size_t* n, FILE* f) {
    if (!lineptr || !n || !f) { errno = EINVAL; return -1; }
    if (!*lineptr || *n < 2) { *n = 128; *lineptr = (char*)malloc(128); }
    if (!*lineptr) { errno = ENOMEM; return -1; }
    ssize_t used = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { if (used == 0) return -1; break; }
        if (used + 2 > (ssize_t)*n) {
            *n *= 2;
            char* nb = (char*)realloc(*lineptr, *n);
            if (!nb) { errno = ENOMEM; return -1; }
            *lineptr = nb;
        }
        (*lineptr)[used++] = (char)c;
        if (c == '\n') break;
    }
    (*lineptr)[used] = '\0';
    return used;
}

// ── fscanf stub ───────────────────────────────────────────────────────────
// Matches 0 items.  libdrm uses it for optional /sys PCI-ID probes;
// callers already tolerate a 0 match count.
int fscanf(FILE* f, const char* fmt, ...) {
    (void)f; (void)fmt;
    return 0;
}

// ── remove ────────────────────────────────────────────────────────────────
// POSIX: unlink for files, rmdir for directories.  We don't have rmdir
// yet — unlink handles both in practice on our FS.
extern int unlink(const char*);
int remove(const char* path) { return unlink(path); }

// ── open_memstream / fmemopen stubs ───────────────────────────────────────
// No in-memory FILE backend yet.  libdrm uses open_memstream for debug
// buffering, harfbuzz/fontconfig for in-memory font loads — they fall
// back cleanly to malloc+write when NULL is returned.
FILE* open_memstream(char** bufp, size_t* sizep) {
    (void)bufp; (void)sizep;
    errno = ENOSYS;
    return (FILE*)0;
}
FILE* fmemopen(void* buf, size_t size, const char* mode) {
    (void)buf; (void)size; (void)mode;
    errno = ENOSYS;
    return (FILE*)0;
}


// ── popen / pclose ───────────────────────────────────────────────────
// Classic pipe + fork + /bin/sh -c.  The child's pid rides in the
// FILE so pclose can reap it and return the wait status.
extern int  pipe(int fds[2]);
extern int  fork(void);
extern int  dup2(int oldfd, int newfd);
extern int  execl(const char* path, const char* arg0, ...);
extern int  waitpid(int pid, int* status, int options);
extern void _exit(int code);

FILE* popen(const char* command, const char* mode) {
    if (!command || !mode) return NULL;
    int read_mode = (mode[0] == 'r');
    int fds[2];
    if (pipe(fds) != 0) return NULL;

    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        if (read_mode) dup2(fds[1], 1);
        else           dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command, (char*)0);
        _exit(127);
    }

    int keep = read_mode ? fds[0] : fds[1];
    close(read_mode ? fds[1] : fds[0]);
    FILE* f = fdopen(keep, mode);
    if (!f) { close(keep); return NULL; }
    f->popen_pid = pid;
    return f;
}

int pclose(FILE* f) {
    if (!f) return -1;
    int pid = f->popen_pid;
    fclose(f);
    if (pid <= 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return status;
}
