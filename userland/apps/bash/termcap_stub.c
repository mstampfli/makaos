/* minimal termcap stubs for bash/readline on MakaOS */
#include <stddef.h>

char PC = 0;
char *BC = 0;
char *UP = 0;

int tgetent(char *bp, const char *name) { (void)bp; (void)name; return 1; }
int tgetflag(const char *id) { (void)id; return 0; }
int tgetnum(const char *id) {
    if (id && id[0]=='l' && id[1]=='i') return 25;  /* lines */
    if (id && id[0]=='c' && id[1]=='o') return 80;  /* cols */
    return -1;
}
char *tgetstr(const char *id, char **area) { (void)id; (void)area; return 0; }
char *tgoto(const char *cap, int col, int row) { (void)cap; (void)col; (void)row; return 0; }
int tputs(const char *str, int affcnt, int (*putc_fn)(int)) {
    if (!str) return 0;
    while (*str) putc_fn((unsigned char)*str++);
    return 0;
}
