/* regex.h - POSIX regcomp/regexec (BRE + ERE, GNU \| \+ \? \w \< \b extensions); constants match glibc */
#pragma once
#include <stddef.h>
typedef int regoff_t;
typedef struct { size_t re_nsub; void *re_prog; } regex_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NEWLINE  4
#define REG_NOSUB    8
#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_STARTEND 4      /* search pmatch[0].rm_so..rm_eo of the string; offsets stay absolute */
enum { REG_NOERROR, REG_NOMATCH, REG_BADPAT, REG_ECOLLATE, REG_ECTYPE, REG_EESCAPE, REG_ESUBREG,
       REG_EBRACK, REG_EPAREN, REG_EBRACE, REG_BADBR, REG_ERANGE, REG_ESPACE, REG_BADRPT };
int regcomp(regex_t *re, const char *pat, int cflags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int err, const regex_t *re, char *buf, size_t size);
void regfree(regex_t *re);
