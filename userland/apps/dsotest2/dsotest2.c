/*
 * dsotest2 -- a second shared object that libdso.so depends on (DT_NEEDED), for
 * the dependency-chain test: dlopen(libdso.so) must load this first so
 * libdso.so's call to dso2_value() binds.  No imports of its own.
 */
int dso2_value(void) { return 99; }
