#ifndef _MAKAOS_STDIO_H
#define _MAKAOS_STDIO_H 1

#include <stddef.h>
#include <stdarg.h>

// Self-contained ssize_t / off_t — libgcc's freestanding compile path
// pulls in <stdio.h> without being able to reach <sys/types.h>, so we
// can't rely on it transitively.  Guards prevent redefinition when the
// caller already pulled in <sys/types.h>.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED 1
typedef long ssize_t;
#endif
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED 1
typedef long long off_t;
#endif

#define EOF   (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 4096
#define FOPEN_MAX    64
#define L_tmpnam     32

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _FILE FILE;
typedef off_t fpos_t;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// Formatted output
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int asprintf(char** strp, const char* fmt, ...);
int vasprintf(char** strp, const char* fmt, va_list ap);

// Formatted input
int scanf(const char* fmt, ...);
int fscanf(FILE* f, const char* fmt, ...);
int sscanf(const char* s, const char* fmt, ...);

// Stream I/O
FILE*  fopen(const char* path, const char* mode);
FILE*  fdopen(int fd, const char* mode);
FILE*  freopen(const char* path, const char* mode, FILE* f);
int    fclose(FILE* f);
int    fflush(FILE* f);
size_t fread(void* buf, size_t sz, size_t n, FILE* f);
size_t fwrite(const void* buf, size_t sz, size_t n, FILE* f);
int    fseek(FILE* f, long off, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);
int    fgetpos(FILE* f, fpos_t* pos);
int    fsetpos(FILE* f, const fpos_t* pos);
int    feof(FILE* f);
int    ferror(FILE* f);
void   clearerr(FILE* f);
int    fileno(FILE* f);

// Character I/O
int  fgetc(FILE* f);
int  fputc(int c, FILE* f);
int  getc(FILE* f);
int  putc(int c, FILE* f);
int  getchar(void);
int  putchar(int c);
int  ungetc(int c, FILE* f);
char* fgets(char* s, int size, FILE* f);
int  fputs(const char* s, FILE* f);
int  puts(const char* s);

// Error messages
void perror(const char* prefix);
void setbuf(FILE* f, char* buf);
int  setvbuf(FILE* f, char* buf, int mode, size_t size);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

// Temp files / rename / remove
int   remove(const char* path);
int   rename(const char* oldp, const char* newp);
FILE* tmpfile(void);
char* tmpnam(char* buf);

// Line-oriented POSIX extensions
ssize_t getline(char** lineptr, size_t* n, FILE* f);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* f);

#endif
