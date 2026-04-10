#pragma once
#include "libc.h"

// ── FILE type ─────────────────────────────────────────────────────────────
// Buffered I/O layer on top of raw fd syscalls.

#define STDIO_BUFSIZ 4096

typedef struct FILE {
    int     fd;          // underlying file descriptor (-1 = closed/invalid)
    int     flags;       // _FILE_READ | _FILE_WRITE | _FILE_ERR | _FILE_EOF
    // Read buffer
    uint8_t rbuf[STDIO_BUFSIZ];
    int     rpos;        // next byte to consume from rbuf
    int     rlen;        // valid bytes in rbuf
    // Write buffer
    uint8_t wbuf[STDIO_BUFSIZ];
    int     wpos;        // next write position in wbuf
    // Position tracking (for ftell; updated on flush/fill)
    long    file_pos;    // current logical position in file
} FILE;

#define _FILE_READ  0x01
#define _FILE_WRITE 0x02
#define _FILE_ERR   0x04
#define _FILE_EOF   0x08

// ── Standard streams ──────────────────────────────────────────────────────
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// ── fopen modes ───────────────────────────────────────────────────────────
FILE*  fopen(const char* path, const char* mode);
FILE*  fdopen(int fd, const char* mode);
int    fclose(FILE* f);

// ── Read / write ──────────────────────────────────────────────────────────
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f);
int    fgetc(FILE* f);
int    fputc(int c, FILE* f);
char*  fgets(char* s, int n, FILE* f);
int    fputs(const char* s, FILE* f);
int    getchar(void);

// ── Seeking ───────────────────────────────────────────────────────────────
int    fseek(FILE* f, long offset, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);

// ── State ─────────────────────────────────────────────────────────────────
int    feof(FILE* f);
int    ferror(FILE* f);
void   clearerr(FILE* f);
int    fflush(FILE* f);

// ── Formatted output ──────────────────────────────────────────────────────

// va_list — GCC builtins
#ifndef _VA_LIST_DEFINED
#define _VA_LIST_DEFINED
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
#endif

int    fprintf(FILE* f, const char* fmt, ...);
int    vfprintf(FILE* f, const char* fmt, va_list ap);
int    vprintf(const char* fmt, va_list ap);

// ── Convenience macros (POSIX) ────────────────────────────────────────────
#define getc(f)    fgetc(f)
#define putc(c,f)  fputc(c,f)

// ── EOF sentinel ──────────────────────────────────────────────────────────
#ifndef EOF
#define EOF (-1)
#endif

// ── fileno ────────────────────────────────────────────────────────────────
static inline int fileno(FILE* f) {
    if (!f) { extern int errno; errno = 9 /*EBADF*/; return -1; }
    return f->fd;
}
