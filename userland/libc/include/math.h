#ifndef _MAKAOS_MATH_H
#define _MAKAOS_MATH_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#ifndef HUGE_VAL
#define HUGE_VAL   __builtin_huge_val()
#endif
#ifndef INFINITY
#define INFINITY   __builtin_inff()
#endif
#ifndef NAN
#define NAN        __builtin_nanf("")
#endif

// C99 classification macros — compiler builtins, no libm call needed.
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, \
                                             FP_NORMAL, FP_SUBNORMAL, \
                                             FP_ZERO, (x))

#define FP_NAN        0
#define FP_INFINITE   1
#define FP_NORMAL     2
#define FP_SUBNORMAL  3
#define FP_ZERO       4

double floor(double);
double ceil(double);
double round(double);
double trunc(double);
double fabs(double);
double fmod(double, double);
double sqrt(double);
double cbrt(double);
double pow(double, double);
double exp(double);
double log(double);
double log2(double);
double log10(double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double sinh(double);
double cosh(double);
double tanh(double);
double hypot(double, double);
double copysign(double, double);
double ldexp(double, int);
double frexp(double, int*);
double modf(double, double*);

// long double variants — alias to double on MakaOS (no 80-bit x87).
long double sqrtl(long double);
long double fabsl(long double);
long double floorl(long double);
long double ceill(long double);
long double truncl(long double);
long double roundl(long double);
long double fmodl(long double, long double);
long double frexpl(long double, int*);
long double ldexpl(long double, int);
long double modfl(long double, long double*);
long double logl(long double);
long double log2l(long double);
long double log10l(long double);
long double expl(long double);
long double powl(long double, long double);
long double sinl(long double);
long double cosl(long double);
long double tanl(long double);
long double asinl(long double);
long double acosl(long double);
long double atanl(long double);
long double atan2l(long double, long double);
long double copysignl(long double, long double);

float  floorf(float);
float  ceilf(float);
float  fabsf(float);
float  sqrtf(float);
float  powf(float, float);
float  expf(float);
float  logf(float);
float  sinf(float);
float  cosf(float);
float  tanf(float);
float  roundf(float);
float  truncf(float);
float  fmodf(float, float);
float  copysignf(float, float);
float  fminf(float, float);
float  fmaxf(float, float);
float  hypotf(float, float);
float  atanf(float);
float  atan2f(float, float);
float  asinf(float);
float  acosf(float);
float  log2f(float);
float  log10f(float);
float  cbrtf(float);
float  sinhf(float);
float  coshf(float);
float  tanhf(float);

double fmin(double, double);
double fmax(double, double);
// (long double variants are declared once in the block above —
// duplicates trip -Werror=redundant-decls consumers like pango.)

#ifdef __cplusplus
}
#endif

#endif
