#ifndef _MAKAOS_DLFCN_H
#define _MAKAOS_DLFCN_H 1

// MakaOS statically links everything — dlopen/dlsym cannot load a new
// library at runtime.  These symbols exist only so code compiled
// against glibc's <dlfcn.h> resolves.  dlopen() always returns NULL,
// dlsym() always returns NULL, dlerror() returns a fixed diagnostic.

#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_NOLOAD   0x00004
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0x00000
#define RTLD_NODELETE 0x01000

#define RTLD_DEFAULT  ((void*)0)
#define RTLD_NEXT     ((void*)-1)

typedef struct {
    const char* dli_fname;
    void*       dli_fbase;
    const char* dli_sname;
    void*       dli_saddr;
} Dl_info;

void* dlopen(const char* file, int flags);
void* dlsym(void* handle, const char* name);
int   dlclose(void* handle);
char* dlerror(void);
int   dladdr(const void* addr, Dl_info* info);

#endif
