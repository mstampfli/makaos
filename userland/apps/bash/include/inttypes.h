#pragma once
#include "libc.h"

/* printf/scanf format macros for fixed-width types */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIi64  "li"
#define PRIu64  "lu"
#define PRIx64  "lx"
#define PRIo64  "lo"
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIuMAX "lu"
#define PRIxMAX "lx"

#define SCNd64  "ld"
#define SCNdMAX "ld"
