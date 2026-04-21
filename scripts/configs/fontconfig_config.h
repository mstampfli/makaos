// Hand-written config.h for fontconfig 2.15.0, bypassing the stock
// autoconf.  x86_64 / GCC / POSIX-ish libc + freetype + libexpat in
// the sysroot.
//
// IMPORTANT: fontconfig sources mix `#ifdef HAVE_X` and `#if HAVE_X`.
// For the `#ifdef` path, any #define (even #define X 0) makes the
// feature appear present.  So "no" means leave UNDEFINED, not
// #define to 0.

#pragma once

#define PACKAGE                  "fontconfig"
#define PACKAGE_NAME             "fontconfig"
#define PACKAGE_VERSION          "2.15.0"
#define VERSION                  "2.15.0"
#define FC_VERSION               "2.15.0"
#define FC_ARCHITECTURE          "x86_64"

// ── Headers we provide ─────────────────────────────────────────────
#define HAVE_STDINT_H            1
#define HAVE_STDLIB_H            1
#define HAVE_STRING_H            1
#define HAVE_SYS_TYPES_H         1
#define HAVE_SYS_STAT_H          1
#define HAVE_SYS_MMAN_H          1
#define HAVE_FCNTL_H             1
#define HAVE_UNISTD_H            1
#define HAVE_DIRENT_H            1
#define HAVE_INTTYPES_H          1
#define HAVE_DLFCN_H             1
#define HAVE_REGEX_H             1
#define STDC_HEADERS             1

// ── Functions we provide ───────────────────────────────────────────
#define HAVE_STRCASECMP          1
#define HAVE_STRDUP              1
#define HAVE_RAND                1
#define HAVE_MMAP                1
#define HAVE_GETOPT              1
#define HAVE_GETPAGESIZE         1
#define HAVE_GNUC_ATTRIBUTE      1
#define HAVE_EXPAT               1
#define HAVE_FREETYPE            1

// ── Compile-time constants ─────────────────────────────────────────
#define ALIGNOF_VOID_P           8
#define ALIGNOF_DOUBLE           8
#define SIZEOF_VOID_P            8
#define FLEXIBLE_ARRAY_MEMBER    /* empty — C99 flex array */

// ── Locations baked into the binary ────────────────────────────────
#define CONFDIR                  "/etc/fonts"
#define CONFIGDIR                "/etc/fonts/conf.d"
#define FC_CACHEDIR              "/var/cache/fontconfig"
#define FC_DEFAULT_FONTS         "/usr/share/fonts"
#define FC_TEMPLATEDIR           "/usr/share/fontconfig/conf.avail"
#define FONTCONFIG_PATH          "/etc/fonts"

// ── XML backend: expat, not libxml2 ────────────────────────────────
#define USE_EXPAT                1

// Everything NOT defined above is automatically "not available"
// (HAVE_SYS_STATVFS_H, HAVE_STRCASESTR, HAVE_RAND_R, HAVE_RANDOM,
//  HAVE_LRAND48, HAVE_MKSTEMP, HAVE_POSIX_FADVISE, HAVE_GETOPT_LONG,
//  HAVE_SCANDIR, HAVE_STRUCT_STATFS_F_FSTYPENAME,
//  HAVE_STRUCT_STATVFS_F_BASETYPE, ENABLE_LIBXML2, HAVE_ICONV, …).
