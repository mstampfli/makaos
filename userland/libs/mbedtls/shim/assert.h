#pragma once
#include "libc.h"
#ifdef NDEBUG
  #define assert(x) ((void)0)
#else
  #define assert(x) do { if (!(x)) { write(2, "assert\n", 7); exit(1); } } while (0)
#endif
