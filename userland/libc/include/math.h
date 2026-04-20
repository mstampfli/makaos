#ifndef _MAKAOS_MATH_H
#define _MAKAOS_MATH_H 1

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_SQRT2    1.41421356237309504880

#define HUGE_VAL   __builtin_huge_val()
#define INFINITY   __builtin_inff()
#define NAN        __builtin_nanf("")

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

#endif
