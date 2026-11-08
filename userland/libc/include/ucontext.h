#ifndef _UCONTEXT_H
#define _UCONTEXT_H
#ifdef __cplusplus
extern "C" {
#endif
// glibc-compatible <ucontext.h> for x86-64.  A SA_SIGINFO handler receives a
// `ucontext_t*` as its third argument; JVMs (Avian, HotSpot) read and MODIFY
// uc_mcontext.gregs[REG_RIP] (advance past a faulting/polling instruction to
// resume) and read REG_RSP/REG_RBP to walk a suspended thread's stack.  The
// kernel populates this from its sigframe on delivery and restores from it
// (honouring handler edits) on sigreturn -- see kernel/proc/signal.c.

#include <stdint.h>
#include <signal.h>

// stack_t (POSIX signal/alt-stack descriptor); also used by sigaltstack.
#ifndef _STACK_T_DEFINED
#define _STACK_T_DEFINED
typedef struct {
    void*         ss_sp;
    int           ss_flags;
    unsigned long ss_size;
} stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE  2
#endif

// gregs[] index names -- MUST match the glibc x86-64 order exactly, because
// ported code indexes gregs[REG_RIP] etc. by these values.
enum {
    REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
    REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2
};
#define NGREG 23

typedef long long greg_t;
typedef greg_t gregset_t[NGREG];

// 512-byte FXSAVE area, glibc-compatible layout. HotSpot's crash handler reads
// fpregs->_xmm[i] and fpregs->mxcsr, so this must be a complete type.
struct _libc_fpxreg { unsigned short significand[4]; unsigned short exponent; unsigned short __pad[3]; };
struct _libc_xmmreg { uint32_t element[4]; };
struct _libc_fpstate {
    uint16_t cwd, swd, ftw, fop;
    uint64_t rip, rdp;
    uint32_t mxcsr, mxcr_mask;
    struct _libc_fpxreg _st[8];
    struct _libc_xmmreg _xmm[16];
    uint32_t __pad[24];
};

typedef struct {
    gregset_t              gregs;
    struct _libc_fpstate*  fpregs;
    unsigned long long     __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
    unsigned long        uc_flags;
    struct ucontext_t*   uc_link;
    stack_t              uc_stack;
    mcontext_t           uc_mcontext;
    sigset_t             uc_sigmask;
    // Trailing FXSAVE area the fpregs pointer refers to.
    struct {
        uint16_t cwd, swd, ftw, fop;
        uint64_t rip, rdp;
        uint32_t mxcsr, mxcr_mask;
        uint32_t st_space[32];
        uint32_t xmm_space[64];
        uint32_t padding[24];
    } __fpregs_mem;
} ucontext_t;

// User-context switching (makecontext/swapcontext) -- not implemented yet;
// declared for source compatibility.  getcontext/setcontext operate on the
// same struct.
int  getcontext(ucontext_t* ucp);
int  setcontext(const ucontext_t* ucp);
void makecontext(ucontext_t* ucp, void (*func)(void), int argc, ...);
int  swapcontext(ucontext_t* oucp, const ucontext_t* ucp);

#ifdef __cplusplus
}
#endif
#endif /* _UCONTEXT_H */
