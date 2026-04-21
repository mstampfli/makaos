#pragma once

// ── Constants ─────────────────────────────────────────────────────────────
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.71828182845904523536
#define M_SQRT2     1.41421356237309504880
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define HUGE_VAL    __builtin_huge_val()
#define NAN         __builtin_nanf("")
#define INFINITY    __builtin_inff()

// ── Basic math (extern — implemented in math.c) ───────────────────────────
// Single-expression wrappers over __builtin_* compile to one SSE2
// instruction (sqrtsd, sqrtss, andpd, roundsd, ...) — no perf loss
// relative to the previous static-inline version.
//
// isnan/isinf/isfinite stay as macros because they're polymorphic over
// float/double/long double in ISO C and must not decay to a single type.

double fabs(double x);
float  fabsf(float x);
double sqrt(double x);
float  sqrtf(float x);
double floor(double x);
float  floorf(float x);
double ceil(double x);
float  ceilf(float x);
double round(double x);
float  roundf(float x);
double trunc(double x);
float  truncf(float x);
double fmin(double a, double b);
double fmax(double a, double b);
float  fminf(float a, float b);
float  fmaxf(float a, float b);
double copysign(double x, double y);
float  copysignf(float x, float y);
double fmod(double x, double y);
float  fmodf(float x, float y);
double hypot(double x, double y);
float  hypotf(float x, float y);
double cbrt(double x);
float  cbrtf(float x);

#define isnan(x)     __builtin_isnan(x)
#define isinf(x)     __builtin_isinf(x)
#define isfinite(x)  __builtin_isfinite(x)

// ── Software implementations (libm) ───────────────────────────────────────
// Forward declarations — implemented in math.c using polynomial approximations
// accurate enough for Doom (which uses fixed-point for most geometry; these
// are for the occasional angle/distance calculation).

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int* exp);
double modf(double x, double* iptr);
double sinh(double x);
double cosh(double x);
double tanh(double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float powf(float x, float y);
float logf(float x);
float expf(float x);

// copysign/copysignf/hypot are declared above as externs; impl in math.c.
