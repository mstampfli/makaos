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
/* misc stubs for bash on MakaOS */
#include <stddef.h>
#include <stdint.h>

void setpwent(void) {}

void bcopy(const void *src, void *dst, size_t n) {
    const char *s = src; char *d = dst;
    if (s < d) { s += n; d += n; while (n--) *--d = *--s; }
    else while (n--) *d++ = *s++;
}

int isascii(int c) { return (unsigned)c < 128; }
int isgraph(int c) { return c > 32 && c < 127; }
char *mkdtemp(char *t) { (void)t; return 0; }

static uint64_t _rng = 12345678901234567ULL;
static uint32_t _r32(void) { _rng^=_rng<<13;_rng^=_rng>>7;_rng^=_rng<<17;return(uint32_t)(_rng>>32); }
void sbrand(unsigned long s) { _rng=s^0xdeadbeefcafeULL; }
void seedrand(unsigned long s) { sbrand(s); }
unsigned long brand(void) { return _r32(); }
uint32_t get_urandom32(void) { return _r32(); }
void seedrand32(uint32_t a,uint32_t b) { _rng=((uint64_t)a<<32)|b; }

int isnetconn(int fd) { (void)fd; return 0; }
int netopen(const char *s) { (void)s; return -1; }

int u32cconv(unsigned int c, char *s) { if(c<0x80){s[0]=c;return 1;} return 0; }
void u32reset(void) {}
char *fnx_fromfs(char *s, size_t n) { (void)n; return s; }

int progcomp_search(const char *s) { (void)s; return 0; }
int progcomp_walk(int (*f)(const char*,void*,int),void *d,int m){(void)f;(void)d;(void)m;return 0;}
int progcomp_insert(const char *s,void *c){(void)s;(void)c;return 0;}
int progcomp_remove(const char *s){(void)s;return 0;}
void progcomp_flush(void){}
int pcomp_set_compspec_options(void *c,int f,int w){(void)c;(void)f;(void)w;return 0;}
int pcomp_set_readline_variables(int f,int o){(void)f;(void)o;return 0;}
void *compspec_create(void){return 0;}
void compspec_dispose(void *c){(void)c;}
void *gen_compspec_completions(void *c,const char *cm,const char *w,int s,int e,int *oc){
    (void)c;(void)cm;(void)w;(void)s;(void)e;if(oc)*oc=0;return 0;}
void *completions_to_stringlist(void *l){(void)l;return 0;}
void *bash_default_completion(const char *t,const char *s,int st,int en,int f){
    (void)t;(void)s;(void)st;(void)en;(void)f;return 0;}
int pcomp_curcs, pcomp_ind;
char *pcomp_curcmd, *pcomp_line;
