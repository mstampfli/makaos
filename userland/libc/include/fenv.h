#ifndef _MAKAOS_FENV_H
#define _MAKAOS_FENV_H 1
// C99 <fenv.h> — floating-point environment.  MakaOS relies on the
// default x86-64 SSE rounding mode (round-to-nearest); these defines
// and the fe*round stubs exist so ports that #include <fenv.h> but
// never actually change the rounding mode compile cleanly.  If a
// port begins to depend on the runtime semantics we'll wire the
// fegetround / fesetround fxsave paths then.

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short fexcept_t;
typedef struct { unsigned int ctrl, status; } fenv_t;

#define FE_INVALID     0x01
#define FE_DIVBYZERO   0x04
#define FE_OVERFLOW    0x08
#define FE_UNDERFLOW   0x10
#define FE_INEXACT     0x20
#define FE_ALL_EXCEPT  (FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW|FE_INEXACT)

#define FE_TONEAREST   0x0000
#define FE_DOWNWARD    0x0400
#define FE_UPWARD      0x0800
#define FE_TOWARDZERO  0x0C00

#define FE_DFL_ENV ((const fenv_t*)0)

int feclearexcept(int excepts);
int feraiseexcept(int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t* env);
int fesetenv(const fenv_t* env);
int feupdateenv(const fenv_t* env);
int fegetexceptflag(fexcept_t* flagp, int excepts);
int fesetexceptflag(const fexcept_t* flagp, int excepts);

#ifdef __cplusplus
}
#endif

#endif
