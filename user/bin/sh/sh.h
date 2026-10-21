/* sh.h - VibeOS shell: shared declarations */
#pragma once
#include <stddef.h>

enum { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_RIN, T_ROUT, T_RAPP, T_RDUP };
struct tok { int type; char *s; int fd, arg; };   /* s: raw word; redirs: fd = target of redirect, arg = dup source */
struct toks { struct tok *v; int n, cap; };
struct vec { char **v; int n, cap; };
struct sb { char *p; size_t n, cap; };

extern int g_status, g_sargc, g_interactive, g_intr;   /* g_intr: ^C at a prompt */
extern char **g_sargv;

/* parse.c */
void sb_add(struct sb *b, int c);
char *sb_take(struct sb *b);
void vec_add(struct vec *v, char *s);
int lex(const char *line, struct toks *t);       /* 0 ok, 1 open quote/operator, 2 trailing backslash */
void toks_free(struct toks *t);
void expand_word(const char *raw, struct vec *out, int split_glob);
char *expand_one(const char *raw);

/* exec.c */
int run_toks(struct toks *t);
int run_source(const char *path, int argc, char **argv);
int run_lines(const char *text);                 /* run a multi-line string (sh -c) */
int run_reader(char *(*next)(void *ctx, int cont), void *ctx);   /* next() -> malloc'd line or NULL */
int is_builtin(const char *name);
const char *find_cmd(const char *name, char *buf, size_t n);   /* NULL if not found */
void reap_jobs(void);
const char *builtin_name(int i);                 /* i-th builtin or NULL */

/* edit.c */
char *readline_(const char *prompt);             /* malloc'd line, NULL on EOF */
void hist_load(void);
void hist_add(const char *line);
int hist_count(void);
const char *hist_get(int i);
void hist_clear(void);
int term_cols(void);
