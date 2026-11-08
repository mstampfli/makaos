// cxxabi_compat.c - C++-mangled aliases for C functions that the MakaOS
// libstdc++.a imports.  That libstdc++ was built against sysroot headers which
// did NOT wrap these in extern "C", so its objects reference the C++-mangled
// names (e.g. _Z4statPKcP4stat = stat(char const*, stat*)).  When libstdc++ is
// turned into a shared libstdc++.so.6 (needed by prebuilt glibc natives like
// LWJGL's libopenal.so), those names must resolve.  Each is a thin forwarder to
// the real C symbol; the launcher whole-archives libc and exports these so the
// .so binds them at load.  Compiled freestanding (no sysroot headers) so the
// generic register-compatible signatures don't clash with the real prototypes.
typedef unsigned long ul;

extern int   stat(const char*, void*);
extern void* opendir(const char*);
extern void* readdir(void*);
extern int   closedir(void*);
extern int   statvfs(const char*, void*);
extern long  ioctl(int, ul, ul);
extern char* nl_langinfo(int);
extern int   fputc(int, void*);

int   _cxx_stat(const char* a, void* b)    __asm__("_Z4statPKcP4stat");
int   _cxx_stat(const char* a, void* b)    { return stat(a, b); }
void* _cxx_opendir(const char* a)          __asm__("_Z7opendirPKc");
void* _cxx_opendir(const char* a)          { return opendir(a); }
void* _cxx_readdir(void* a)                __asm__("_Z7readdirP4_DIR");
void* _cxx_readdir(void* a)                { return readdir(a); }
int   _cxx_closedir(void* a)               __asm__("_Z8closedirP4_DIR");
int   _cxx_closedir(void* a)               { return closedir(a); }
int   _cxx_statvfs(const char* a, void* b) __asm__("_Z7statvfsPKcP7statvfs");
int   _cxx_statvfs(const char* a, void* b) { return statvfs(a, b); }
long  _cxx_ioctl(int a, ul b, ul c)        __asm__("_Z5ioctlimz");
long  _cxx_ioctl(int a, ul b, ul c)        { return ioctl(a, b, c); }
char* _cxx_nllang(int a)                   __asm__("_Z11nl_langinfoi");
char* _cxx_nllang(int a)                   { return nl_langinfo(a); }

// putc: not exported by our stdio (it's a macro there); real fputc exists.
int putc(int c, void* f) { return fputc(c, f); }
