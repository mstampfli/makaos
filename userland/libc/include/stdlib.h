#ifndef _MAKAOS_STDLIB_H
#define _MAKAOS_STDLIB_H 1

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF
#define MB_CUR_MAX   4

// Memory
void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t new_size);
void* calloc(size_t nmemb, size_t size);
void* aligned_alloc(size_t alignment, size_t size);

// Conversion
int        atoi(const char* s);
long       atol(const char* s);
long long  atoll(const char* s);
double     atof(const char* s);
long       strtol(const char* s, char** endptr, int base);
long long  strtoll(const char* s, char** endptr, int base);
unsigned long      strtoul(const char* s, char** endptr, int base);
unsigned long long strtoull(const char* s, char** endptr, int base);
double     strtod(const char* s, char** endptr);
float      strtof(const char* s, char** endptr);

// Env
char* getenv(const char* name);
int   setenv(const char* name, const char* value, int overwrite);
int   unsetenv(const char* name);
int   putenv(char* str);

// Process control
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
__attribute__((noreturn)) void abort(void);
int  atexit(void (*fn)(void));
int  system(const char* cmd);

// Random (CSPRNG-backed via /dev/urandom)
int  rand(void);
void srand(unsigned int seed);

// Search / sort
void  qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t n, size_t sz,
               int (*cmp)(const void*, const void*));

// Absolute value
int       abs(int v);
long      labs(long v);
long long llabs(long long v);

typedef struct { long long quot; long long rem; } imaxdiv_t;
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t     div(int n, int d);
ldiv_t    ldiv(long n, long d);
lldiv_t   lldiv(long long n, long long d);
imaxdiv_t imaxdiv(long long n, long long d);

#endif
