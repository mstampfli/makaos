// ── dlfcn.c — dynamic-linker API stubs ───────────────────────────────
//
// MakaOS statically links every binary.  dlopen/dlsym cannot load a
// new object at runtime.  These symbols exist so ported code that
// probes for optional plugins (mbedTLS/PKCS11, fontconfig language
// probes, etc.) gets a clean NULL answer instead of a link failure.

#include <dlfcn.h>

static const char s_dl_msg[] = "dlopen: dynamic loading is not supported on MakaOS";

void* dlopen(const char* file, int flags) { (void)file; (void)flags; return 0; }
void* dlsym(void* h, const char* name)     { (void)h; (void)name; return 0; }
int   dlclose(void* h)                     { (void)h; return 0; }
char* dlerror(void)                        { return (char*)s_dl_msg; }

int dladdr(const void* addr, Dl_info* info) {
    (void)addr;
    if (info) {
        info->dli_fname = 0;
        info->dli_fbase = 0;
        info->dli_sname = 0;
        info->dli_saddr = 0;
    }
    return 0;
}
