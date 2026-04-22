#ifndef _MAKAOS_ASSERT_H
#define _MAKAOS_ASSERT_H 1

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
  #define assert(x) ((void)0)
#else
  __attribute__((__noreturn__)) void __assert_fail(const char* expr, const char* file,
                                                int line, const char* func);
  #define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

#ifdef __cplusplus
}
#endif

// C11 static_assert alias.  C++ already has static_assert as a
// keyword and C23 promotes it to a keyword too — only define the
// compat macro outside both worlds, and let callers (foot) win if
// they've pre-defined it.
#if !defined(__cplusplus) && !defined(static_assert)
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define static_assert _Static_assert
#endif
#endif

#endif
