// Minimal expat_config.h for the MakaOS sysroot.  Normally generated
// by autoconf; we bypass the ./configure dance entirely.  Values
// reflect a POSIX-ish little-endian target with a POSIX libc.

#pragma once

#define PACKAGE           "expat"
#define PACKAGE_BUGREPORT "expat-bugs@libexpat.org"
#define PACKAGE_NAME      "expat"
#define PACKAGE_STRING    "expat 2.6.4"
#define PACKAGE_TARNAME   "expat"
#define PACKAGE_URL       ""
#define PACKAGE_VERSION   "2.6.4"
#define VERSION           "2.6.4"

#define HAVE_DLFCN_H      1
#define HAVE_FCNTL_H      1
#define HAVE_INTTYPES_H   1
#define HAVE_STDINT_H     1
#define HAVE_STDIO_H      1
#define HAVE_STDLIB_H     1
#define HAVE_STRING_H     1
#define HAVE_STRINGS_H    1
#define HAVE_SYS_STAT_H   1
#define HAVE_SYS_TYPES_H  1
#define HAVE_UNISTD_H     1
#define HAVE_GETPAGESIZE  1
#define HAVE_MMAP         1
#define HAVE_MEMMOVE      1

#define STDC_HEADERS      1

#define BYTEORDER         1234  // little-endian x86_64

// expat internals
#define XML_NS            1
#define XML_DTD           1
#define XML_GE            1     // general entities (required by public API)
#define XML_CONTEXT_BYTES 1024
#define XML_POOR_ENTROPY  1     // use fallback randomness — no getrandom

// xmlparse.c uses isnan() but doesn't include <math.h>; it relies on
// autotools to splice it in.  Pre-include here.
#include <math.h>
