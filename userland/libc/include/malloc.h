// malloc.h - malloc + glibc malloc extensions (compatible subset).
//
// The core allocator lives in the C library; the extension functions
// (malloc_trim/mallinfo/malloc_stats/malloc_usable_size) are provided so
// portable code that includes <malloc.h> compiles and links. MakaOS's
// allocator does not expose trimming or per-block size introspection yet, so
// those report "nothing to do" / unknown (see libc.c).

#ifndef _MALLOC_H
#define _MALLOC_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void*  malloc(size_t size);
void*  calloc(size_t nmemb, size_t size);
void*  realloc(void* ptr, size_t size);
void   free(void* ptr);
void*  memalign(size_t alignment, size_t size);

size_t malloc_usable_size(void* ptr);
int    malloc_trim(size_t pad);
void   malloc_stats(void);

struct mallinfo {
    int arena;    int ordblks; int smblks;  int hblks;
    int hblkhd;   int usmblks; int fsmblks;  int uordblks;
    int fordblks; int keepcost;
};
struct mallinfo mallinfo(void);

#ifdef __cplusplus
}
#endif

#endif /* malloc.h */
