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

// ── SSE2 intrinsics via scalar asm ────────────────────────────────────────
// These are tiny wrappers that emit a single SSE2 scalar instruction.

static inline double fabs(double x) {
    // Clear the sign bit via SSE2 andpd.
    double r;
    __asm__("andpd %1, %0" : "=x"(r) : "x"(x), "0"(x));
    // Simpler: use the builtin which compiles to andpd anyway.
    return __builtin_fabs(x);
}

static inline float fabsf(float x) { return __builtin_fabsf(x); }

static inline double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline double floor(double x) { return __builtin_floor(x); }
static inline float  floorf(float x) { return __builtin_floorf(x); }
static inline double ceil(double x)  { return __builtin_ceil(x); }
static inline float  ceilf(float x)  { return __builtin_ceilf(x); }
static inline double round(double x) { return __builtin_round(x); }
static inline float  roundf(float x) { return __builtin_roundf(x); }
static inline double trunc(double x) { return __builtin_trunc(x); }

static inline int    isnan(double x)   { return __builtin_isnan(x); }
static inline int    isinf(double x)   { return __builtin_isinf(x); }
static inline int    isfinite(double x){ return __builtin_isfinite(x); }
static inline double fmin(double a, double b) { return a < b ? a : b; }
static inline double fmax(double a, double b) { return a > b ? a : b; }
static inline double fmod(double x, double y) {
    return x - (double)(long long)(x / y) * y;
}
static inline float  fmodf(float x, float y) {
    return x - (float)(long long)(x / y) * y;
}

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

// ── POSIX: copysign ───────────────────────────────────────────────────────
static inline double copysign(double x, double y) {
    return __builtin_copysign(x, y);
}
static inline float copysignf(float x, float y) {
    return __builtin_copysignf(x, y);
}

// ── POSIX: hypot ──────────────────────────────────────────────────────────
static inline double hypot(double x, double y) {
    return sqrt(x*x + y*y);
}
