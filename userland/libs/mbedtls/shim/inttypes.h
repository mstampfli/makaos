#pragma once
#include "libc.h"

// mbedTLS uses PRIu32, PRIu64 etc. in debug logs (which we keep off),
// but also pattern-matches PRIx* for cert printing in tests.  Provide
// the minimum gcc-compatible set.
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIdMAX "lld"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
