#ifndef _MAKAOS_ASSERT_H
#define _MAKAOS_ASSERT_H 1

#ifdef NDEBUG
  #define assert(x) ((void)0)
#else
  __attribute__((noreturn)) void __assert_fail(const char* expr, const char* file,
                                                int line, const char* func);
  #define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

// C11 static_assert alias.
#define static_assert _Static_assert

#endif
