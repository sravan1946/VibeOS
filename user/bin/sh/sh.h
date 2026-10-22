/* sh.h - VibeOS shell: shared declarations */
#pragma once
#include <stddef.h>

struct vec { char **v; int n, cap; };
struct sb { char *p; size_t n, cap; };

/* ---- AST (parse.c); every node and string lives in the prog's arena ---- */
enum { N_CMD, N_PIPE, N_AND, N_OR, N_NOT, N_LIST, N_BG, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_GROUP, N_SUB, N_FUNC };
enum { R_IN, R_OUT, R_APP, R_DUP, R_HERE };
struct redir { struct redir *next; int type, fd, arg, quoted; char *word; };   /* R_HERE: word = body */
struct citem { struct citem *next; char **pats; struct node *body; };
struct node {
    int type, nw, hasin;
    struct node *a, *b, *c;     /* children: AND/OR a,b; IF cond/then/else; WHILE cond/body; FOR/GROUP body in a */
    struct node *next;          /* LIST items and PIPE stages are chained through next */
    char **w;                   /* CMD words / FOR list (raw, unexpanded), NULL-terminated */
    char *s;                    /* FOR variable, FUNC name, SUB inner text, CASE word */
    char *src;                  /* source text of the command (for running it in a child shell) */
    struct redir *rd;
    struct citem *items;
};
struct arena;
struct prog { struct arena *ar; struct node *root; };

enum { P_OK, P_INCOMPLETE, P_ERROR };
extern char g_perr[160];

/* control flow after break/continue/return */
enum { CTL_NONE, CTL_BREAK, CTL_CONT, CTL_RET };
extern int g_status, g_sargc, g_interactive, g_intr, g_ctl, g_ctln, g_errexit, g_xtrace, g_noerr, g_lastbg;
extern char **g_sargv;                           /* [0] = $0; malloc'd, see set_pos() */

/* parse.c */
int parse_text(const char *text, struct prog *out);   /* P_OK / P_INCOMPLETE / P_ERROR (message in g_perr) */
void prog_free(struct prog *p);
int skip_paren(const char *s, int j);            /* s[j] just after "$(": index after the matching ')' or -1 */
int skip_brace(const char *s, int j);            /* s[j] just after "${": index after '}' or -1 */
int skip_bq(const char *s, int j);               /* s[j] just after '`' */
void sb_add(struct sb *b, int c);
void sb_adds(struct sb *b, const char *s);
char *sb_take(struct sb *b);
void vec_add(struct vec *v, char *s);

/* expand.c */
enum { M_ARGV, M_STR, M_PAT };                   /* field-split+glob / single string / glob pattern */
void expand_word(const char *raw, struct vec *out, int mode);
char *expand_one(const char *raw);
char *expand_pat(const char *raw);
char *expand_here(const char *raw);              /* here-doc body: $ ` \ only */
int gmatch(const char *pat, const char *s);
const char *var_get(const char *name);           /* NULL if unset */
void var_set(const char *name, const char *val); /* keeps the exported flag */
void var_export(const char *name, const char *val);   /* val NULL: export the current value */
void var_unset(const char *name);
int var_exported(const char *name);
int is_name(const char *s, int len);
char **var_envp(struct vec *prefix, int with_locals);   /* malloc'd array (strings are not owned) */
void var_list(int (*fn)(const char *name, const char *val, int exported));
void set_pos(int argc, char **argv);             /* replace $0..$n with copies; argv[0] is $0 */
long arith(const char *expr);

/* exec.c */
int run_node(struct node *n);
int run_text(const char *text);                  /* parse + run (eval) */
int run_source(const char *path, int argc, char **argv);
int run_lines(const char *text);                 /* sh -c */
int run_reader(char *(*next)(void *ctx, int cont), void *ctx);   /* next() -> malloc'd line or NULL */
char *cmdsub(const char *text);                  /* $(...) : output with trailing newlines stripped */
int spawn_sh(const char *text, int fds[3], int *pid);   /* child "sh -c text" carrying our state */
int wait_pid(int pid);                           /* status, EINTR-safe */
extern volatile uint32_t g_trap_pending;         /* signals whose trap hasn't run yet */
void run_traps(void);
int  trap_set(int sig);                          /* a trap (or ignore) is installed */
void trap_raise(int sig);
int call_function(const char *name, int argc, char **argv, int *found);
int fn_define(const char *name, const char *src);
int fn_exists(const char *name);
void fn_unset(const char *name);
void fn_list(void (*fn)(const char *name, const char *src));
void reap_jobs(void);
int jobs_wait(int pid);                          /* wait [pid]; 0 pid = all */
void jobs_print(void);
void local_mark(int *mark);
void local_release(int mark);
void local_declare(const char *name);

/* builtin.c */
int is_builtin(const char *name);
int run_builtin(int argc, char **argv);
const char *find_cmd(const char *name, char *buf, size_t n);   /* NULL if not found */
const char *builtin_name(int i);                 /* i-th builtin or NULL */
const char *alias_get(const char *name);
void set_options(const char *flags);

/* edit.c */
char *readline_(const char *prompt);             /* malloc'd line, NULL on EOF */
void hist_load(void);
void hist_add(const char *line);
int hist_count(void);
const char *hist_get(int i);
void hist_clear(void);
int term_cols(void);
char *hist_expand(const char *line, int *changed);   /* !! !prefix !N ; NULL = event not found */
