/*
 * dsotest -- a minimal no-import shared object for the dlopen (Phase 2)
 * milestone.  Built with `ld -shared` -> ET_DYN + dynsym + one R_X86_64_RELATIVE
 * (the s_name pointer).  Freestanding: it imports nothing, so the loader only
 * has to map it and apply the RELATIVE reloc.
 */
static const char *volatile s_name = "libdso";   /* -> R_X86_64_RELATIVE */

/* strlen is UNDEFINED in this .so -> a JUMP_SLOT/GLOB_DAT reloc the loader must
 * bind against the main executable's exported strlen (milestone 2). */
extern unsigned long strlen(const char*);

/* A WEAK undefined symbol: no object provides it, so it must resolve to 0 and
 * dlopen must still SUCCEED (loader hardening -- weak symbols are not fatal). */
extern void dso_absent_weak(void) __attribute__((weak));

/* dso2_value lives in libdso2.so, a DT_NEEDED dependency of this .so. */
extern int dso2_value(void);

int dso_answer(void) { return 42; }
const char* dso_name(void) { return s_name; }     /* returns the relocated pointer */
int dso_strlen(void) { return (int)strlen("abcd"); }   /* calls libc via the exe scope */
void* dso_weak(void) { return (void*)dso_absent_weak; }  /* == 0 when weak-unresolved */
int dso_dep(void) { return dso2_value() + 1; }    /* calls into the DT_NEEDED dep */
