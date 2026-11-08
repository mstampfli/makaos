// math_c99.c - the C99/long-double math functions the host libstdc++ <cmath>
// and <cstdlib> expect a complete libc to provide. MakaOS's math.c already has
// the core double/float set (cosh, cbrt, hypot, rint, ...); this fills in the
// missing C99 doubles, all the long-double (l) variants, and the float (f)
// variants of the new doubles. The l variants delegate to the double routines
// (MakaOS carries no extended-precision path); callers that pull these in via
// libstdc++ do not need 80-bit accuracy. Compiled with -msse2 like math.c.

#include <limits.h>   // INT_MIN / INT_MAX

// Base routines (all defined in math.c and archived into libc.a). Declared here
// directly so this file does not depend on which of MakaOS's two <math.h>
// variants (internal userland/libc/math.h vs the fuller sysroot header) is on
// the include path -- the internal one omits nearbyint/rint/fma.
extern double log(double), sqrt(double), exp(double), sin(double), fabs(double);
extern double ldexp(double, int);
extern double frexp(double, int*);
extern double nearbyint(double), rint(double), round(double);
extern float  rintf(float), roundf(float);
extern double cosh(double), sinh(double), tanh(double), cbrt(double);
extern double hypot(double, double), fma(double, double, double);
extern double fmax(double, double), fmin(double, double);
extern double exp2(double);

#define M_PI      3.14159265358979323846
#define NAN       (__builtin_nanf(""))
#define isinf(x)  __builtin_isinf(x)
#define isnan(x)  __builtin_isnan(x)

// ── missing C99 doubles ──────────────────────────────────────────────────────
double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }
double asinh(double x) { return log(x + sqrt(x * x + 1.0)); }
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }

double fdim(double x, double y) { return (x > y) ? x - y : 0.0; }

double scalbn(double x, int n)   { return ldexp(x, n); }        // FLT_RADIX == 2
double scalbln(double x, long n) { return ldexp(x, (int)n); }

double logb(double x) { int e; (void)frexp(x, &e); return (double)(e - 1); }
int    ilogb(double x) {
    if (x == 0.0) return INT_MIN;                 // FP_ILOGB0
    if (isinf(x)) return INT_MAX;
    if (isnan(x)) return INT_MIN;
    int e; (void)frexp(x, &e); return e - 1;
}

double nextafter(double x, double y) {
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    union { double d; unsigned long long u; } v = { x };
    if (x == 0.0) { v.u = 1; return (y > 0.0) ? v.d : -v.d; }
    if ((x < y) == (x > 0.0)) v.u++; else v.u--;
    return v.d;
}
double nexttoward(double x, long double y) { return nextafter(x, (double)y); }

double remainder(double x, double y) {
    if (y == 0.0 || isinf(x) || isnan(x) || isnan(y)) return x + y - (x + y);  // NaN
    double n = nearbyint(x / y);
    return x - n * y;
}
double remquo(double x, double y, int* quo) {
    double n = nearbyint(x / y);
    int q = (int)n;
    if (quo) *quo = ((x < 0) ^ (y < 0)) ? -(q & 7) : (q & 7);
    return x - n * y;
}

// lrint/llrint/lround/llround (double + float) already live in math.c.

double nan(const char* tag) { (void)tag; return (double)NAN; }

double expm1(double x) { return exp(x) - 1.0; }
double log1p(double x) { return log(1.0 + x); }

// erf/erfc: Abramowitz & Stegun 7.1.26 (|err| < 1.5e-7). Not used by the JVM;
// present so libstdc++'s <cmath> links.
double erf(double x) {
    double t = 1.0 / (1.0 + 0.3275911 * fabs(x));
    double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t
                       - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return (x >= 0.0) ? y : -y;
}
double erfc(double x) { return 1.0 - erf(x); }

// lgamma/tgamma: Lanczos approximation (g=7, n=9).
double lgamma(double x) {
    static const double c[9] = {
        0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7 };
    if (x < 0.5) return log(M_PI / fabs(sin(M_PI * x))) - lgamma(1.0 - x);
    x -= 1.0;
    double a = c[0];
    double t = x + 7.5;
    for (int i = 1; i < 9; i++) a += c[i] / (x + (double)i);
    return 0.5 * log(2.0 * M_PI) + (x + 0.5) * log(t) - t + log(a);
}
double tgamma(double x) {
    if (x < 0.5) return M_PI / (sin(M_PI * x) * tgamma(1.0 - x));
    return exp(lgamma(x));
}

// ── float variants of the new functions ─────────────────────────────────────
float acoshf(float x)  { return (float)acosh((double)x); }
float asinhf(float x)  { return (float)asinh((double)x); }
float atanhf(float x)  { return (float)atanh((double)x); }
float fdimf(float x, float y) { return (float)fdim((double)x, (double)y); }
float erff(float x)    { return (float)erf((double)x); }
float erfcf(float x)   { return (float)erfc((double)x); }
float lgammaf(float x) { return (float)lgamma((double)x); }
float tgammaf(float x) { return (float)tgamma((double)x); }
float logbf(float x)   { return (float)logb((double)x); }
int   ilogbf(float x)  { return ilogb((double)x); }
float scalbnf(float x, int n)   { return (float)scalbn((double)x, n); }
float scalblnf(float x, long n) { return (float)scalbln((double)x, n); }
float nextafterf(float x, float y) { return (float)nextafter((double)x, (double)y); }
float nexttowardf(float x, long double y) { return (float)nextafter((double)x, (double)y); }
float remainderf(float x, float y) { return (float)remainder((double)x, (double)y); }
float remquof(float x, float y, int* q) { return (float)remquo((double)x, (double)y, q); }
float nanf(const char* tag) { (void)tag; return (float)NAN; }
float expm1f(float x) { return (float)expm1((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }

// ── long-double variants (delegate to double) ───────────────────────────────
long double acoshl(long double x)  { return (long double)acosh((double)x); }
long double asinhl(long double x)  { return (long double)asinh((double)x); }
long double atanhl(long double x)  { return (long double)atanh((double)x); }
long double coshl(long double x)   { return (long double)cosh((double)x); }
long double sinhl(long double x)   { return (long double)sinh((double)x); }
long double tanhl(long double x)   { return (long double)tanh((double)x); }
long double cbrtl(long double x)   { return (long double)cbrt((double)x); }
long double hypotl(long double x, long double y) { return (long double)hypot((double)x, (double)y); }
long double fmal(long double x, long double y, long double z) { return (long double)fma((double)x, (double)y, (double)z); }
long double fmaxl(long double x, long double y) { return (long double)fmax((double)x, (double)y); }
long double fminl(long double x, long double y) { return (long double)fmin((double)x, (double)y); }
long double rintl(long double x)      { return (long double)rint((double)x); }
long double nearbyintl(long double x) { return (long double)nearbyint((double)x); }
long double fdiml(long double x, long double y) { return (long double)fdim((double)x, (double)y); }
long double erfl(long double x)    { return (long double)erf((double)x); }
long double erfcl(long double x)   { return (long double)erfc((double)x); }
long double lgammal(long double x) { return (long double)lgamma((double)x); }
long double tgammal(long double x) { return (long double)tgamma((double)x); }
long double logbl(long double x)   { return (long double)logb((double)x); }
int         ilogbl(long double x)  { return ilogb((double)x); }
long double scalbnl(long double x, int n)   { return (long double)scalbn((double)x, n); }
long double scalblnl(long double x, long n) { return (long double)scalbln((double)x, n); }
long double nextafterl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double nexttowardl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
long double remainderl(long double x, long double y) { return (long double)remainder((double)x, (double)y); }
long double remquol(long double x, long double y, int* q) { return (long double)remquo((double)x, (double)y, q); }
long        lrintl(long double x)   { return (long)rint((double)x); }
long long   llrintl(long double x)  { return (long long)rint((double)x); }
long        lroundl(long double x)  { return (long)round((double)x); }
long long   llroundl(long double x) { return (long long)round((double)x); }
long double nanl(const char* tag)   { (void)tag; return (long double)NAN; }
long double expm1l(long double x)   { return (long double)expm1((double)x); }
long double log1pl(long double x)   { return (long double)log1p((double)x); }
long double exp2l(long double x)    { return (long double)exp2((double)x); }
