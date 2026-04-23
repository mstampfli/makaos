#ifndef _MAKAOS_SETJMP_H
#define _MAKAOS_SETJMP_H 1

// Callee-saved register snapshot + rip/rsp + mxcsr.  Layout must
// match userland/libc/setjmp.asm exactly:
//   [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15
//   [6] rsp  [7] rip  [8] mxcsr
// When this was 8 slots (64 bytes) instead of 9 (72 bytes), every
// setjmp() caller that declared a jmp_buf as the *first field* of a
// struct got its next field silently clobbered by stmxcsr, and
// longjmp() read a stale 4 bytes back as MXCSR.  In freetype's
// gray_TWorker the next field is `num_cells`, and the resulting 4-
// byte tail overflow + subsequent ldmxcsr of garbage manifested as
// RBP corruption after ft_smooth_render's internal callback returned
// (observed via Alt+Shift+Enter → foot PF-KILL in glyph rasterize).
typedef struct { long __jb[9]; } jmp_buf[1];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);
__attribute__((__noreturn__)) void longjmp(jmp_buf env, int val);
int  sigsetjmp(sigjmp_buf env, int save_mask);
__attribute__((__noreturn__)) void siglongjmp(sigjmp_buf env, int val);
#define _setjmp   setjmp
#define _longjmp  longjmp

#endif
