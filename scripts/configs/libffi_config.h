// Minimal fficonfig.h-equivalent for x86_64 MakaOS.  Normally
// libffi's autoconf generates fficonfig.h with feature probes; we
// short-circuit because we know our target.

#pragma once

#define HAVE_AS_X86_64_UNWIND_SECTION_TYPE 1
#define HAVE_AS_X86_PCREL                  1
#define HAVE_DLFCN_H                       1
#define HAVE_INTTYPES_H                    1
#define HAVE_MEMCPY                        1
#define HAVE_MEMORY_H                      1
#define HAVE_MMAP                          1
#define HAVE_MMAP_ANON                     1
#define HAVE_MMAP_DEV_ZERO                 0
#define HAVE_MMAP_FILE                     1
#define HAVE_PTRDIFF_T                     1
#define HAVE_STDINT_H                      1
#define HAVE_STDLIB_H                      1
#define HAVE_STRING_H                      1
#define HAVE_STRINGS_H                     1
#define HAVE_SYS_MMAN_H                    1
#define HAVE_SYS_STAT_H                    1
#define HAVE_SYS_TYPES_H                   1
#define HAVE_UNISTD_H                      1
#define HAVE_LONG_DOUBLE                   1
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE   1

#define STDC_HEADERS                       1

#define SIZEOF_DOUBLE                      8
#define SIZEOF_LONG_DOUBLE                 16
#define SIZEOF_SIZE_T                      8

#define PACKAGE                            "libffi"
#define PACKAGE_NAME                       "libffi"
#define PACKAGE_STRING                     "libffi 3.4.6"
#define PACKAGE_TARNAME                    "libffi"
#define PACKAGE_VERSION                    "3.4.6"
#define VERSION                            "3.4.6"

// Calling convention: System V on Linux/MakaOS, NOT Windows.
#define USE_BUILTIN_FFS                    0
#define X86_64                             1

// We ship static; libffi distinguishes static-only by absence of
// FFI_GO_CLOSURES — we don't define it.

// AH_BOTTOM: FFI_HIDDEN visibility marker, normally produced by
// configure.ac.  Different shape for C vs assembly — assembly TUs
// define LIBFFI_ASM before including fficonfig.h.
#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
# ifdef LIBFFI_ASM
#  define FFI_HIDDEN(name) .hidden name
# else
#  define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
# endif
#else
# ifdef LIBFFI_ASM
#  define FFI_HIDDEN(name)
# else
#  define FFI_HIDDEN
# endif
#endif
