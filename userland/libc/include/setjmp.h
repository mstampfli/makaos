#ifndef _MAKAOS_SETJMP_H
#define _MAKAOS_SETJMP_H 1

// Callee-saved register snapshot + rip/rsp.  Layout must match
// userland/libc/setjmp.asm.
typedef struct { long __jb[8]; } jmp_buf[1];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);
int  sigsetjmp(sigjmp_buf env, int save_mask);
__attribute__((noreturn)) void siglongjmp(sigjmp_buf env, int val);
#define _setjmp   setjmp
#define _longjmp  longjmp

#endif
