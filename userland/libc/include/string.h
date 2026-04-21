#ifndef _MAKAOS_STRING_H
#define _MAKAOS_STRING_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// POSIX puts ffs/bzero/strcasecmp in <strings.h>; many ports use
// them without including strings.h explicitly.  Pull it in here so
// every TU that #includes <string.h> gets them transitively.
#include <strings.h>

// Memory block
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
void*  memchr(const void* s, int c, size_t n);

// C strings
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t max);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
size_t strlcpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
char*  strncat(char* dst, const char* src, size_t n);
size_t strlcat(char* dst, const char* src, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);

// BSD aliases kept for portability — libinput and a few other legacy
// ports still reach for them.  Semantically identical to strchr/strrchr.
char*  index (const char* s, int c);
char*  rindex(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char*  strtok(char* s, const char* delim);
char*  strtok_r(char* s, const char* delim, char** saveptr);
char*  strdup(const char* s);
char*  strndup(const char* s, size_t n);
char*  strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif
