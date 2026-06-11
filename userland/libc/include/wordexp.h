// ── wordexp.h — shell-style word expansion ───────────────────────────
// Subset implementation for sway's config-path expansion
// (common/stringop.c expand_path): tilde expansion + $VAR/${VAR}
// substitution + whitespace field splitting.  No command substitution
// (WRDE_NOCMD is implicitly always on), no arithmetic, no globbing —
// a pattern that matches nothing stays literal, which sway treats as
// "config file not found" and continues.

#ifndef _MAKAOS_WORDEXP_H
#define _MAKAOS_WORDEXP_H 1

#include <stddef.h>

typedef struct {
    size_t we_wordc;   // number of expanded words
    char** we_wordv;   // NULL-terminated word vector
    size_t we_offs;    // reserved slots (WRDE_DOOFFS)
} wordexp_t;

// Flags — accepted; only WRDE_NOCMD/WRDE_UNDEF alter behaviour.
#define WRDE_APPEND  0x01
#define WRDE_DOOFFS  0x02
#define WRDE_NOCMD   0x04
#define WRDE_REUSE   0x08
#define WRDE_SHOWERR 0x10
#define WRDE_UNDEF   0x20

// Error returns
#define WRDE_BADCHAR 1
#define WRDE_BADVAL  2
#define WRDE_CMDSUB  3
#define WRDE_NOSPACE 4
#define WRDE_SYNTAX  5

int  wordexp(const char* words, wordexp_t* we, int flags);
void wordfree(wordexp_t* we);

#endif
