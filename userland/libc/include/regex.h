#ifndef _MAKAOS_REGEX_H
#define _MAKAOS_REGEX_H 1

#include <sys/types.h>

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8

#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12

typedef struct {
    void*  __internal;
    size_t re_nsub;
} regex_t;

typedef struct {
    off_t rm_so;
    off_t rm_eo;
} regmatch_t;

int  regcomp(regex_t* preg, const char* pattern, int cflags);
int  regexec(const regex_t* preg, const char* string,
              size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t* preg, char* buf, size_t size);
void regfree(regex_t* preg);

#endif
