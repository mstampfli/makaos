#pragma once
#include "libc.h"
#ifndef offsetof
  #define offsetof(t, m) __builtin_offsetof(t, m)
#endif
