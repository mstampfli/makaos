// Hand-written config.h for pixman, replacing what autoconf would
// generate.  x86_64 little-endian, no SIMD backends compiled in
// (generic implementation only — add MMX/SSE2/SSSE3 when wlroots
// shows we need them).

#pragma once

#define PACKAGE                  "pixman"
#define PACKAGE_BUGREPORT        ""
#define PACKAGE_NAME             "pixman"
#define PACKAGE_STRING           "pixman 0.42.2"
#define PACKAGE_TARNAME          "pixman"
#define PACKAGE_URL              ""
#define PACKAGE_VERSION          "0.42.2"
#define VERSION                  "0.42.2"

#define HAVE_STDINT_H            1
#define HAVE_STDLIB_H            1
#define HAVE_STRING_H            1
#define HAVE_SYS_TYPES_H         1
#define HAVE_UNISTD_H            1
#define HAVE_DLFCN_H             1
#define HAVE_INTTYPES_H          1
#define HAVE_FENV_H              1
#define HAVE_FLOAT_H             1
#define HAVE_GETISAX             0
#define HAVE_PIXMAN_GLYPH_CACHE  1
#define HAVE_POSIX_MEMALIGN      0       // we have aligned_alloc-equivalent paths instead
#define HAVE_PTHREADS            1
#define HAVE_GCC_VECTOR_EXTENSIONS 1
#define HAVE_FEDISABLEEXCEPT     0       // not in our libm

#define STDC_HEADERS             1

// Endianness
#define WORDS_BIGENDIAN_NOT      1   // we are little-endian; the
                                      // negation here is a sentinel —
                                      // pixman checks WORDS_BIGENDIAN
                                      // (undefined on LE).

// SIMD: nothing.  Generic C path covers all blends; SIMD is a
// performance optimisation we can layer on later.
// #define USE_X86_MMX
// #define USE_SSE2
// #define USE_SSSE3

// Identity stubs for the per-arch implementation probes are NOT in
// the config header (forward-decl ordering issues).  Provided in a
// separate stub TU compiled into libpixman-1.a — see port-pixman.sh.

// Compiler hints
#define PIXMAN_EXPORT            __attribute__ ((visibility ("default")))

// Thread-local storage: GCC supports __thread natively on x86_64,
// no pthread_key_t dance needed.  Pixman picks up this branch via
// `#elif defined(TLS)` in pixman-compiler.h.
#define TLS                      __thread
