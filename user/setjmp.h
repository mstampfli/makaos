#pragma once

// jmp_buf: stores rbx, rbp, r12-r15, rsp, rip, mxcsr (9 × 8 bytes = 72 bytes)
typedef unsigned long long jmp_buf[9];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

// POSIX aliases
#define _setjmp  setjmp
#define _longjmp longjmp
