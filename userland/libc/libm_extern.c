// ── libm extern symbols for sysroot consumers ───────────────────────
//
// libc/math.h has the basic math functions as `static inline` so
// in-tree apps that include libc.h inline them directly.  But ported
// libraries linked against the sysroot's <math.h> (which only declares
// these functions extern) need real link symbols.  This file provides
// them.
//
// We intentionally do NOT include libc/math.h here — its static-inline
// definitions would collide with these extern definitions at the C
// language level.  Each function is a one-liner over a __builtin_*
// (compiler emits the right instruction directly: sqrtsd, sqrtss,
// roundsd, etc.).
//
// Long double variants alias to double; MakaOS doesn't ship 80-bit
// libm, x86_64 long double precision is not exploited by any port we
// care about.

#include <stddef.h>

double sqrt(double x)        { return __builtin_sqrt(x); }
float  sqrtf(float x)        { return __builtin_sqrtf(x); }
double fabs(double x)        { return __builtin_fabs(x); }
float  fabsf(float x)        { return __builtin_fabsf(x); }
double floor(double x)       { return __builtin_floor(x); }
float  floorf(float x)       { return __builtin_floorf(x); }
double ceil(double x)        { return __builtin_ceil(x); }
float  ceilf(float x)        { return __builtin_ceilf(x); }
double round(double x)       { return __builtin_round(x); }
float  roundf(float x)       { return __builtin_roundf(x); }
double trunc(double x)       { return __builtin_trunc(x); }
float  truncf(float x)       { return __builtin_truncf(x); }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }
float  fminf(float a, float b)  { return a < b ? a : b; }
float  fmaxf(float a, float b)  { return a > b ? a : b; }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
float  copysignf(float x, float y)  { return __builtin_copysignf(x, y); }
double fmod(double x, double y)     { return __builtin_fmod(x, y); }
float  fmodf(float x, float y)      { return __builtin_fmodf(x, y); }

long double sqrtl(long double x)  { return (long double)__builtin_sqrt((double)x); }
long double fabsl(long double x)  { return (long double)__builtin_fabs((double)x); }
long double floorl(long double x) { return (long double)__builtin_floor((double)x); }
long double ceill(long double x)  { return (long double)__builtin_ceil((double)x); }
long double fmodl(long double x, long double y) {
    return (long double)__builtin_fmod((double)x, (double)y);
}

// Additional float variants that libm.h declares.  Each wraps the
// double version via explicit cast — single-precision on x86 would
// use sqrtss/cvtss2sd under the hood; for these single-use paths the
// precision is fine.
extern double atan(double x);
extern double atan2(double y, double x);
extern double asin(double x);
extern double acos(double x);
extern double log2(double x);
extern double log10(double x);
extern double cbrt(double x);
extern double sinh(double x);
extern double cosh(double x);
extern double tanh(double x);
extern double hypot(double x, double y);

float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
float atanf(float x)           { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float asinf(float x)           { return (float)asin((double)x); }
float acosf(float x)           { return (float)acos((double)x); }
float log2f(float x)           { return (float)log2((double)x); }
float log10f(float x)          { return (float)log10((double)x); }
float cbrtf(float x)           { return (float)cbrt((double)x); }
float sinhf(float x)           { return (float)sinh((double)x); }
float coshf(float x)           { return (float)cosh((double)x); }
float tanhf(float x)           { return (float)tanh((double)x); }
