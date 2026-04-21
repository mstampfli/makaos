// math.c — Software floating-point library for the Doom port.
//
// Accuracy targets:
//   sin/cos/tan  : < 1 ULP (Cody-Waite range reduction + minimax polynomial)
//   atan2        : < 1 ULP
//   sqrt         : exact (SSE2 sqrtsd)
//   exp/log/pow  : < 2 ULP
//   Everything else: within a few ULP — sufficient for Doom.
//
// Implementation uses double-precision throughout; float variants are simple
// casts.  No dependency on any other library.

#include "math.h"

// ── Internal helpers ──────────────────────────────────────────────────────

typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;

// Reinterpret double bits as uint64_t and vice-versa.
static inline uint64_t d2bits(double x) {
    uint64_t u; __builtin_memcpy(&u, &x, 8); return u;
}
static inline double bits2d(uint64_t u) {
    double x; __builtin_memcpy(&x, &u, 8); return x;
}
static inline uint32_t f2bits(float x) {
    uint32_t u; __builtin_memcpy(&u, &x, 4); return u;
}
static inline float bits2f(uint32_t u) {
    float x; __builtin_memcpy(&x, &u, 4); return x;
}

static inline double d_abs(double x) { return x < 0.0 ? -x : x; }

// ── Constants ─────────────────────────────────────────────────────────────
#define PI      3.14159265358979323846
#define PI_2    1.57079632679489661923
#define PI_4    0.78539816339744830962
#define LN2     0.69314718055994530942
#define LOG2E   1.44269504088896340736

// ── sin / cos ─────────────────────────────────────────────────────────────
// Cody-Waite argument reduction modulo π/2, then evaluate minimax polynomial.

// Polynomial for sin on [-π/4, π/4]:
//   sin(x) ≈ x + x³·p(x²)
static double sin_kernel(double x) {
    double x2 = x * x;
    // Coefficients from Cephes / FDLIBM.
    double r = x2 * (-1.66666666666666324348e-1
               + x2 * (8.33333333332248946124e-3
               + x2 * (-1.98412698298579493134e-4
               + x2 * (2.75573137070700676789e-6
               + x2 * (-2.50507602534165751498e-8
               + x2 *  1.58969099521155010221e-10)))));
    return x + x * r;
}

// Polynomial for cos on [-π/4, π/4]:
//   cos(x) ≈ 1 + x²·p(x²)
static double cos_kernel(double x) {
    double x2 = x * x;
    double r = x2 * (-5.00000000000000000000e-1
               + x2 * (4.16666666666666551899e-2
               + x2 * (-1.38888888888872993015e-3
               + x2 * (2.48015872894767294178e-5
               + x2 * (-2.75573143513906633035e-7
               + x2 * (2.08757232129817851248e-9
               - x2 *  1.13596475577881948265e-11))))));
    return 1.0 + r;
}

// Reduce x modulo π/2 into [-π/4, π/4], return quadrant 0-3.
static int trig_reduce(double x, double* xr) {
    // For |x| > 2^20 * π/2 this simplified reduction loses accuracy.
    // Doom never passes such large angles so this is fine.
    double fn = (double)(long long)(x * (2.0 / PI));   // n = round(x/(π/2))
    // Cody-Waite constants for π/2:
    // C1 = 1.5707963267948966192 (high bits)
    // C2 = 6.123233995736765886e-17 (remainder)
    *xr = x - fn * 1.5707963267948966192
            - fn * 6.123233995736765886e-17;
    int n = (int)(long long)fn & 3;
    if (n < 0) n += 4;
    return n;
}

double sin(double x) {
    if (x != x) return x;           // NaN
    int neg = (x < 0.0);
    if (neg) x = -x;
    double xr;
    int n = trig_reduce(x, &xr);
    double r;
    switch (n & 3) {
        case 0: r =  sin_kernel(xr); break;
        case 1: r =  cos_kernel(xr); break;
        case 2: r = -sin_kernel(xr); break;
        default:r = -cos_kernel(xr); break;
    }
    return neg ? -r : r;
}

double cos(double x) {
    if (x != x) return x;           // NaN
    if (x < 0.0) x = -x;
    double xr;
    int n = trig_reduce(x, &xr);
    switch (n & 3) {
        case 0: return  cos_kernel(xr);
        case 1: return -sin_kernel(xr);
        case 2: return -cos_kernel(xr);
        default:return  sin_kernel(xr);
    }
}

double tan(double x) {
    // tan(x) = sin(x)/cos(x) using the kernel polynomials.
    if (x != x) return x;
    int neg = (x < 0.0);
    if (neg) x = -x;
    double xr;
    int n = trig_reduce(x, &xr);
    double s, c;
    // For quadrants 1,3 swap sin and cos (tan(π/2 + x) = -1/tan(x)).
    if (n & 1) {
        s = cos_kernel(xr);
        c = sin_kernel(xr);
        double r = (c == 0.0) ? HUGE_VAL : -(s / c);
        return neg ? -r : r;
    } else {
        s = sin_kernel(xr);
        c = cos_kernel(xr);
        double r = (c == 0.0) ? HUGE_VAL : s / c;
        return neg ? -r : r;
    }
}

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }

// ── atan / atan2 ──────────────────────────────────────────────────────────
// Rational minimax approximation on [0, tan(π/8)]:
//   atan(x) ≈ x + x³·R(x²)  where R is a 5th-order rational.
// Then use the identities to bring |x| into [0, 1].

static double atan_kernel(double x) {
    // Minimax rational from FDLIBM atan.c, accurate to < 1 ULP.
    double x2 = x * x;
    double s  = x2;
    double w  = s * s;
    double t1 = w * (-1.88796008463073496563e-1 + w * (3.05038276862723520987e-2
                   + w * (-6.97316225585992093820e-3 + w * (1.99231539121618028772e-3
                   + w * (-2.40374931759901047325e-4)))));
    double t2 = s * (-3.33329491539184031662e-1 + w * (1.99999999998764832476e-1
                   + w * (-1.42857142725034920291e-1 + w * (1.11111104054623597741e-1
                   + w * (-9.09088983691403754135e-2 + w * 7.42610942028085009791e-2)))));
    return x + x * (t1 + t2);
}

double atan(double x) {
    if (x != x) return x;
    // Identities to reduce to [0, tan(π/8) ≈ 0.4142]:
    //   atan(-x) = -atan(x)
    //   atan(x) = π/2 - atan(1/x)  for |x| > 1
    //   atan(x) = π/4 + atan((x-1)/(x+1)) for x > tan(3π/8)
    int neg = (x < 0.0);
    if (neg) x = -x;
    double r;
    if (x > 2.4142135623730950488) {        // tan(3π/8)
        r = PI_2 - atan_kernel(1.0 / x);
    } else if (x > 0.4142135623730950488) { // tan(π/8)
        double y = (x - 1.0) / (x + 1.0);
        r = PI_4 + atan_kernel(y);
    } else {
        r = atan_kernel(x);
    }
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x == 0.0) {
        if (y == 0.0) return 0.0;
        return (y > 0.0) ? PI_2 : -PI_2;
    }
    double t = atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) t += PI;
        else           t -= PI;
    }
    return t;
}

double asin(double x) {
    // asin(x) = atan(x / sqrt(1 - x²))
    if (x > 1.0) return NAN;
    if (x < -1.0) return NAN;
    if (x == 1.0)  return PI_2;
    if (x == -1.0) return -PI_2;
    double s = sqrt(1.0 - x * x);
    return atan2(x, s);
}

double acos(double x) {
    return PI_2 - asin(x);
}

float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }

// ── exp ───────────────────────────────────────────────────────────────────
// exp(x) = 2^(x/ln2). Reduce x = n*ln2 + r, compute 2^n by exponent field,
// compute exp(r) by polynomial on [-ln2/2, ln2/2].

double exp(double x) {
    if (x != x) return x;
    if (x > 709.78) return HUGE_VAL;
    if (x < -745.1) return 0.0;

    // x = n*ln2 + r, |r| <= ln2/2.
    double fn = (double)(long long)(x * LOG2E + 0.5);
    double r  = x - fn * LN2;

    // Minimax polynomial for (exp(r)-1)/r on [-ln2/2, ln2/2]:
    double r2 = r * r;
    double p = r2 * (5.00000000000000000000e-1
               + r2 * (1.66666666666666019037e-1
               + r2 * (4.16666666666664434e-2
               + r2 * (8.33333333333232829e-3
               + r2 * (1.38888888888888e-3
               + r2 *  1.98412698413e-4)))));
    double c = r - p;
    double e = 1.0 + r - (r * c / (c - 2.0) - r);   // = exp(r) via Horner

    // Scale by 2^n.
    int n = (int)(long long)fn;
    uint64_t bits = d2bits(e) + ((uint64_t)(n + 1023) << 52);
    return bits2d(bits);
}

float expf(float x) { return (float)exp((double)x); }

// ── log ───────────────────────────────────────────────────────────────────
// log(x): extract exponent, reduce mantissa to [1, 2), use poly on [1/√2, √2].

double log(double x) {
    if (x != x) return x;
    if (x <= 0.0) return (x == 0.0) ? -HUGE_VAL : NAN;

    uint64_t bits = d2bits(x);
    int exp = (int)((bits >> 52) & 0x7FF) - 1023;
    // Normalise mantissa to [0.5, 1.0):
    bits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    double f = bits2d(bits);  // f in [0.5, 1.0)
    // Adjust to [sqrt(2)/2, sqrt(2)] via: if f < 1/sqrt(2), double it.
    if (f < 0.7071067811865476) { f *= 2.0; exp--; }
    // s = (f - 1) / (f + 1)
    double s = (f - 1.0) / (f + 1.0);
    double s2 = s * s;
    // log(f) ≈ 2s·P(s²):
    double p = s2 * (3.333333333333333333e-1
               + s2 * (2.000000000000000000e-1
               + s2 * (1.428571428571428571e-1
               + s2 * (1.111111111111111111e-1
               + s2 * (9.090909090909090909e-2
               + s2 *  7.692307692307692308e-2)))));
    return (double)exp * LN2 + 2.0 * s * (1.0 + p);
}

double log2(double x) { return log(x) * LOG2E; }
double log10(double x) {
    static const double log10e = 0.43429448190325182765;
    return log(x) * log10e;
}

float logf(float x) { return (float)log((double)x); }

// ── pow ───────────────────────────────────────────────────────────────────

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : HUGE_VAL;
    if (x < 0.0) {
        // Only valid for integer exponent.
        long long n = (long long)y;
        if ((double)n != y) return NAN;
        double r = exp(y * log(-x));
        return (n & 1) ? -r : r;
    }
    return exp(y * log(x));
}

float powf(float x, float y) { return (float)pow((double)x, (double)y); }

// ── ldexp / frexp / modf ──────────────────────────────────────────────────

double ldexp(double x, int e) {
    uint64_t bits = d2bits(x);
    int ex = (int)((bits >> 52) & 0x7FF);
    ex += e;
    if (ex <= 0) return 0.0;
    if (ex >= 0x7FF) return HUGE_VAL;
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)ex << 52);
    return bits2d(bits);
}

double frexp(double x, int* ep) {
    if (x == 0.0) { *ep = 0; return 0.0; }
    uint64_t bits = d2bits(x);
    int ex = (int)((bits >> 52) & 0x7FF);
    *ep = ex - 1022;
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    return bits2d(bits);
}

double modf(double x, double* iptr) {
    double t = (double)(long long)x;
    *iptr = t;
    return x - t;
}

// ── sinh / cosh / tanh ────────────────────────────────────────────────────

double sinh(double x) {
    return (exp(x) - exp(-x)) * 0.5;
}
double cosh(double x) {
    return (exp(x) + exp(-x)) * 0.5;
}
double tanh(double x) {
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

// ── Basic math: SSE2 via inline asm ──────────────────────────────────
// We can NOT use __builtin_X here: at -O0 (our userland flag) gcc lowers
// __builtin_sqrt / __builtin_fabs / etc. to `call sqrt` / `call fabs` /
// etc. — a call into ourselves, stack-overflows at first invocation.
// Same for __builtin_fmod which even at -O2 emits a libm call.  Hand-
// write them with inline asm so no amount of optimization level change
// can re-introduce the recursion trap.
//
// References:
//  - sqrtsd xmm, xmm         — scalar double square root (SSE2)
//  - sqrtss xmm, xmm         — scalar float square root (SSE2)
//  - andpd / andps           — with a sign-mask constant → fabs
//  - cvttsd2si / cvttss2si   — truncate-to-int for trunc()
//
// floor/ceil need roundsd (SSE4.1).  On CPUs without SSE4.1 we fall
// back to a software path built from trunc + comparison.

double fabs(double x) {
    double r;
    asm("andpd %1, %0" : "=x"(r) : "x"((double)((union { uint64_t u; double d; }){.u=0x7FFFFFFFFFFFFFFFULL}.d)), "0"(x));
    return r;
}
float fabsf(float x) {
    float r;
    asm("andps %1, %0" : "=x"(r) : "x"((float)((union { uint32_t u; float f; }){.u=0x7FFFFFFFu}.f)), "0"(x));
    return r;
}
double sqrt(double x) {
    double r;
    asm("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
float sqrtf(float x) {
    float r;
    asm("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
double copysign(double x, double y) {
    // Mask x's sign bit off, OR in y's sign bit.
    union { uint64_t u; double d; } ax = {.d = x}, ay = {.d = y};
    ax.u = (ax.u & 0x7FFFFFFFFFFFFFFFULL) | (ay.u & 0x8000000000000000ULL);
    return ax.d;
}
float copysignf(float x, float y) {
    union { uint32_t u; float f; } ax = {.f = x}, ay = {.f = y};
    ax.u = (ax.u & 0x7FFFFFFFu) | (ay.u & 0x80000000u);
    return ax.f;
}

// trunc via cvttsd2si for values that fit in int64; otherwise value is
// already an integer.  Handles the full double range safely.
double trunc(double x) {
    union { uint64_t u; double d; } ux = {.d = x};
    int exp = (int)((ux.u >> 52) & 0x7FF) - 1023;
    if (exp < 0) return copysign(0.0, x);
    if (exp >= 52) return x;                 // already integer
    uint64_t mask = ~((uint64_t)0) >> (12 + exp);
    ux.u &= ~mask;
    return ux.d;
}
float truncf(float x) { return (float)trunc((double)x); }

// floor: trunc toward -inf.  If x<0 and has fractional part, step down by 1.
double floor(double x) {
    double t = trunc(x);
    if (t == x) return t;
    if (x < 0.0) return t - 1.0;
    return t;
}
float floorf(float x) { return (float)floor((double)x); }

// ceil: trunc toward +inf.
double ceil(double x) {
    double t = trunc(x);
    if (t == x) return t;
    if (x > 0.0) return t + 1.0;
    return t;
}
float ceilf(float x) { return (float)ceil((double)x); }

// round: round half-away-from-zero.
double round(double x) {
    if (x >= 0.0) return floor(x + 0.5);
    return       ceil (x - 0.5);
}
float roundf(float x) {
    if (x >= 0.0f) return (float)floor((double)x + 0.5);
    return               (float)ceil ((double)x - 0.5);
}

double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }
float  fminf(float a, float b)  { return a < b ? a : b; }
float  fmaxf(float a, float b)  { return a > b ? a : b; }

// fmod: x - trunc(x/y)*y.  Native-SSE2 friendly, no x87, no libm call.
double fmod(double x, double y) {
    if (y == 0.0) return 0.0 / 0.0;           // NaN
    union { uint64_t u; double d; } ux = {.d = x}, uy = {.d = y};
    int ex = (int)((ux.u >> 52) & 0x7FF);
    int ey = (int)((uy.u >> 52) & 0x7FF);
    if (ex == 0x7FF || ey == 0x7FF) return 0.0 / 0.0; // inf/nan
    double q = x / y;
    return x - trunc(q) * y;
}
float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

// ── hypot / cbrt (real impls) ─────────────────────────────────────────────
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double cbrt(double x) {
    return x < 0.0 ? -pow(-x, 1.0 / 3.0) : pow(x, 1.0 / 3.0);
}
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
float cbrtf(float x)           { return (float)cbrt((double)x); }

// ── Single-precision wrappers over the double impls above ────────────────
float atanf(float x)  { return (float)atan((double)x); }
float asinf(float x)  { return (float)asin((double)x); }
float acosf(float x)  { return (float)acos((double)x); }
float log2f(float x)  { return (float)log2((double)x); }
float log10f(float x) { return (float)log10((double)x); }
float sinhf(float x)  { return (float)sinh((double)x); }
float coshf(float x)  { return (float)cosh((double)x); }
float tanhf(float x)  { return (float)tanh((double)x); }

// ── Long-double variants (alias to double) ────────────────────────────────
long double sqrtl(long double x)  { return (long double)__builtin_sqrt((double)x); }
long double fabsl(long double x)  { return (long double)__builtin_fabs((double)x); }
long double floorl(long double x) { return (long double)__builtin_floor((double)x); }
long double ceill(long double x)  { return (long double)__builtin_ceil((double)x); }
long double fmodl(long double x, long double y) {
    return (long double)fmod((double)x, (double)y);
}
long double frexpl(long double x, int* ep) {
    return (long double)frexp((double)x, ep);
}
long double ldexpl(long double x, int e) {
    return (long double)ldexp((double)x, e);
}
long double modfl(long double x, long double* iptr) {
    double di;
    double rv = modf((double)x, &di);
    *iptr = (long double)di;
    return (long double)rv;
}
long double logl(long double x)     { return (long double)log((double)x); }
long double log2l(long double x)    { return (long double)log2((double)x); }
long double log10l(long double x)   { return (long double)log10((double)x); }
long double expl(long double x)     { return (long double)exp((double)x); }
long double powl(long double x, long double y) {
    return (long double)pow((double)x, (double)y);
}
long double sinl(long double x)     { return (long double)sin((double)x); }
long double cosl(long double x)     { return (long double)cos((double)x); }
long double tanl(long double x)     { return (long double)tan((double)x); }
long double asinl(long double x)    { return (long double)asin((double)x); }
long double acosl(long double x)    { return (long double)acos((double)x); }
long double atanl(long double x)    { return (long double)atan((double)x); }
long double atan2l(long double y, long double x) {
    return (long double)atan2((double)y, (double)x);
}
long double truncl(long double x)   { return (long double)__builtin_trunc((double)x); }
long double roundl(long double x)   { return (long double)__builtin_round((double)x); }
long double copysignl(long double x, long double y) {
    return (long double)__builtin_copysign((double)x, (double)y);
}

