#ifndef _MAKAOS_STRINGS_H
#define _MAKAOS_STRINGS_H 1

#include <stddef.h>

int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
void   bzero(void* s, size_t n);
int    ffs(int i);

#endif
