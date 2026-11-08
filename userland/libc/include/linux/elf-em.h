// linux/elf-em.h - ELF machine (EM_*) numbers, as portable code that classifies
// a loaded object's target architecture expects (e.g. OpenJDK os_linux.cpp).

#ifndef _LINUX_ELF_EM_H
#define _LINUX_ELF_EM_H 1

#define EM_NONE     0
#define EM_M32      1
#define EM_SPARC    2
#define EM_386      3
#define EM_68K      4
#define EM_88K      5
#define EM_486      6
#define EM_860      7
#define EM_MIPS     8
#define EM_MIPS_RS3_LE 10
#define EM_PARISC   15
#define EM_ALPHA    41
#define EM_SPARC32PLUS 18
#define EM_PPC      20
#define EM_PPC64    21
#define EM_S390     22
#define EM_ARM      40
#define EM_SH       42
#define EM_SPARCV9  43
#define EM_IA_64    50
#define EM_X86_64   62
#define EM_AARCH64  183
#define EM_RISCV    243
#define EM_LOONGARCH 258

#endif /* linux/elf-em.h */
