/* Shim: redirect to MakaOS libc */
#ifndef _MAKAOS_STDIO_SHIM_H
#define _MAKAOS_STDIO_SHIM_H
#include "libc.h"
/* stdio.h in libc defines FILE, stdin/stdout/stderr, fopen, etc.
   Use include_next to skip this shim and find the libc one. */
#include_next <stdio.h>
#endif
