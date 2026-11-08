// link.h - dynamic-linker introspection (Linux-compatible subset).
//
// Provides struct dl_phdr_info + dl_iterate_phdr, as used by portable code
// (OpenJDK's os_linux.cpp maps a code address to its shared-object name for
// stack traces). MakaOS's loader does not yet expose its link map, so
// dl_iterate_phdr currently visits nothing; see dlfcn.c.

#ifndef _LINK_H
#define _LINK_H 1

#include <elf.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dl_phdr_info {
    Elf64_Addr         dlpi_addr;       /* module load bias */
    const char*        dlpi_name;       /* module path */
    const Elf64_Phdr*  dlpi_phdr;       /* program headers */
    Elf64_Half         dlpi_phnum;      /* number of program headers */
    unsigned long long dlpi_adds;       /* number of load events */
    unsigned long long dlpi_subs;       /* number of unload events */
    size_t             dlpi_tls_modid;
    void*              dlpi_tls_data;
};

// ElfW(type) selects the native-width ELF type (this port is LP64-only).
#define ElfW(type) Elf64_##type

// Dynamic-linker link-map node (as returned by dlinfo(RTLD_DI_LINKMAP)).
struct link_map {
    Elf64_Addr       l_addr;   /* module load bias */
    char*            l_name;   /* module path */
    void*            l_ld;     /* module .dynamic (Elf64_Dyn*) */
    struct link_map* l_next;
    struct link_map* l_prev;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info* info, size_t size,
                                    void* data),
                    void* data);

#ifdef __cplusplus
}
#endif

#endif /* link.h */
