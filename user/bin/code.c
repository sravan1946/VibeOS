/* code - a small IDE: project sidebar, tabbed editor with C / Lua highlighting, undo/redo, find/replace,
 * auto-indent, and Build (tcc, Ctrl+B) / Run (F5) with an output pane that captures stdout and stderr.
 * Clicking a compiler error jumps to file:line. */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syscall.h>

#define MAX_FILE (1 << 20)
#define MAX_TABS 16
#define TAB 4
#define OUT_MAX (96 * 1024)
#define SIDE_W 224
#define TB_H 42
#define TABS_H 34
#define SB_H 26
#define MAX_MARKS 64

enum { L_TEXT, L_C, L_LUA };
enum { H_DEF, H_KW, H_TYPE, H_NUM, H_STR, H_COM, H_PRE, H_FUN, H_N };

#define C_EDIT   RGB(0x12, 0x11, 0x1C)
#define C_GUTTER RGB(0x16, 0x15, 0x22)
#define C_LINENO RGB(0x5E, 0x5A, 0x7C)
static const color_t HCOL[H_N] = {
    RGB(0xED, 0xEB, 0xFF), RGB(0xC7, 0x92, 0xFF), RGB(0x5B, 0xD8, 0xF5), RGB(0xFF, 0xA2, 0x4B),
    RGB(0x7A, 0xE0, 0x9A), RGB(0x6F, 0x6C, 0x93), RGB(0xFF, 0x7A, 0xC6), RGB(0x8C, 0xB4, 0xFF),
};

/* ---------------------------------------------------------------- syntax highlighting */
static const char *const C_KW[] = { "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern", "for",
    "goto", "if", "inline", "register", "return", "sizeof", "static", "struct", "switch", "typedef", "union", "volatile", "while", NULL };
static const char *const C_TY[] = { "int", "char", "short", "long", "float", "double", "void", "unsigned", "signed", "bool", "FILE",
    "size_t", "ssize_t", "va_list", NULL };
static const char *const C_CONST[] = { "NULL", "true", "false", "EOF", "stdin", "stdout", "stderr", NULL };
static const char *const LUA_KW[] = { "and", "break", "do", "else", "elseif", "end", "for", "function", "goto", "if", "in", "local",
    "not", "or", "repeat", "return", "then", "until", "while", NULL };
static const char *const LUA_CONST[] = { "true", "false", "nil", "self", NULL };

static bool in_list(const char *s, int n, const char *const *l) {
    for (; *l; l++) if ((int)strlen(*l) == n && !memcmp(*l, s, n)) return true;
    return false;
}
static bool idstart(int c) { return isalpha(c & 255) || c == '_'; }
static bool idchar(int c) { return isalnum(c & 255) || c == '_'; }
static bool isword(int c) { return idchar(c); }
static bool has(const char *s, int n, const char *needle) {        /* needle occurs in s[0..n) */
    int m = strlen(needle);
    for (int i = 0; i + m <= n; i++) if (!memcmp(s + i, needle, m)) return true;
    return false;
}

/* Colours one line into col[0..n). `st` is the state at the line start (C: 1 = inside a block comment; Lua: 1 + level = long
 * comment, 100 + level = long string); returns the state at its end. */
static int hl_line(int lang, const char *s, int n, int st, unsigned char *col) {
    memset(col, H_DEF, n);
    if (lang == L_TEXT) return 0;
    bool first = true;
    int i = 0;
    while (i < n) {
        if (lang == L_C && st == 1) {
            int j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            if (j < n) { j += 2; st = 0; }
            memset(col + i, H_COM, j - i); i = j; first = false;
            continue;
        }
        if (lang == L_LUA && st) {
            int lvl = st >= 100 ? st - 100 : st - 1, j = i;
            for (; j < n; j++)
                if (s[j] == ']') {
                    int k = j + 1;
                    while (k < n && s[k] == '=') k++;
                    if (k < n && s[k] == ']' && k - j - 1 == lvl) { j = k + 1; st = 0; break; }
                }
            memset(col + i, st >= 100 ? H_STR : H_COM, j - i); i = j;
            continue;
        }
        int c = (unsigned char)s[i];
        if (isspace(c)) { i++; continue; }
        if (lang == L_C) {
            if (c == '/' && i + 1 < n && s[i + 1] == '*') { st = 1; memset(col + i, H_COM, 2); i += 2; first = false; continue; }
            if (c == '/' && i + 1 < n && s[i + 1] == '/') { memset(col + i, H_COM, n - i); return 0; }
            if (c == '#' && first) {
                int j = i;
                while (j < n && !(s[j] == '/' && j + 1 < n && (s[j + 1] == '/' || s[j + 1] == '*'))) j++;
                memset(col + i, H_PRE, j - i); i = j; first = false;
                continue;
            }
        } else {
            if (c == '-' && i + 1 < n && s[i + 1] == '-') {
                int k = i + 2;
                if (k < n && s[k] == '[') {
                    int m = k + 1;
                    while (m < n && s[m] == '=') m++;
                    if (m < n && s[m] == '[') { st = 1 + (m - k - 1); memset(col + i, H_COM, m + 1 - i); i = m + 1; continue; }
                }
                memset(col + i, H_COM, n - i);
                return 0;
            }
            if (c == '[') {
                int m = i + 1;
                while (m < n && s[m] == '=') m++;
                if (m < n && s[m] == '[') { st = 100 + (m - i - 1); memset(col + i, H_STR, m + 1 - i); i = m + 1; continue; }
            }
        }
        first = false;
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\') j++; j++; }
            if (j < n) j++;
            if (j > n) j = n;
            memset(col + i, H_STR, j - i); i = j;
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            int j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.')) j++;
            memset(col + i, H_NUM, j - i); i = j;
            continue;
        }
        if (idstart(c)) {
            int j = i + 1;
            while (j < n && idchar(s[j])) j++;
            int len = j - i, cls = H_DEF;
            if (lang == L_C) {
                if (in_list(s + i, len, C_KW)) cls = H_KW;
                else if (in_list(s + i, len, C_TY) || (len > 2 && s[j - 2] == '_' && s[j - 1] == 't')) cls = H_TYPE;
                else if (in_list(s + i, len, C_CONST)) cls = H_NUM;
            } else {
                if (in_list(s + i, len, LUA_KW)) cls = H_KW;
                else if (in_list(s + i, len, LUA_CONST)) cls = H_NUM;
            }
            if (cls == H_DEF) {
                int k = j;
                while (k < n && s[k] == ' ') k++;
                if (k < n && s[k] == '(') cls = H_FUN;
            }
            memset(col + i, cls, len); i = j;
            continue;
        }
        i++;
    }
    return st;
}

/* ---------------------------------------------------------------- documents */
struct edit { int pos, ndel, nins, grp; char *del, *ins; };
struct doc {
    char path[PATH_MAX];               /* "" = untitled */
    char *buf; int len, cap;
    int cur, anchor, want_col;
    bool modified;
    int top, hx;                       /* first visible line, horizontal pixel scroll */
    int lang;
    int *lines, nlines, lcap;          /* line start offsets, rebuilt lazily */
    unsigned char *lst;                /* highlighter state at each line start */
    bool dirty;
    unsigned ver;                      /* bumped on every edit (find cache) */
    struct edit *undo, *redo; int nundo, ucap, nredo, rcap;
    int grp, lastop;                   /* undo group of the current command; 1 typing, 2 backspace, 3 delete */
};

static struct doc *docs[MAX_TABS];
static int ndocs, curdoc;
static struct doc *d;                  /* current document (NULL when none is open) */
static int grp_ctr;
static int CW, LH;

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static int lang_of(const char *p) {
    const char *dot = strrchr(base(p), '.');
    if (!dot) return L_TEXT;
    if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) return L_C;
    if (!strcmp(dot, ".lua")) return L_LUA;
    return L_TEXT;
}

static void reindex(struct doc *x) {
    if (!x->dirty && x->lines) return;
    static unsigned char scol[4096];
    x->dirty = false;
    int nl = 1;
    for (int i = 0; i < x->len; i++) if (x->buf[i] == '\n') nl++;
    if (nl > x->lcap) {
        x->lcap = nl + 256;
        x->lines = realloc(x->lines, x->lcap * sizeof *x->lines);
        x->lst = realloc(x->lst, x->lcap);
    }
    int n = 0;
    x->lines[n++] = 0;
    for (int i = 0; i < x->len; i++) if (x->buf[i] == '\n') x->lines[n++] = i + 1;
    x->nlines = nl;
    int st = 0;
    for (int l = 0; l < nl; l++) {
        x->lst[l] = st;
        int s = x->lines[l], e = l + 1 < nl ? x->lines[l + 1] - 1 : x->len, len = e - s;
        st = hl_line(x->lang, x->buf + s, len > 4096 ? 4096 : len, st, scol);
    }
}

static int line_of(struct doc *x, int off) {
    reindex(x);
    int lo = 0, hi = x->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (x->lines[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}
static int line_end(struct doc *x, int l) { reindex(x); return l + 1 < x->nlines ? x->lines[l + 1] - 1 : x->len; }

static int vcol(struct doc *x, int l, int off) {
    int v = 0;
    for (int i = x->lines[l]; i < off; i++) v = x->buf[i] == '\t' ? (v / TAB + 1) * TAB : v + 1;
    return v;
}
static int off_at_vcol(struct doc *x, int l, int vc) {
    int v = 0, e = line_end(x, l);
    for (int i = x->lines[l]; i < e; i++) {
        if (vc <= v) return i;
        v = x->buf[i] == '\t' ? (v / TAB + 1) * TAB : v + 1;
    }
    return e;
}

static struct doc *doc_new(const char *path) {
    struct doc *x = calloc(1, sizeof *x);
    x->cap = 4096;
    x->buf = malloc(x->cap);
    x->buf[0] = 0;
    x->dirty = true;
    x->lang = L_C;                     /* untitled buffers start as C */
    if (path) { snprintf(x->path, sizeof x->path, "%s", path); x->lang = lang_of(path); }
    return x;
}

static void free_edits(struct edit *e, int n) {
    for (int i = 0; i < n; i++) { free(e[i].del); free(e[i].ins); }
}
static void doc_free(struct doc *x) {
    free_edits(x->undo, x->nundo); free_edits(x->redo, x->nredo);
    free(x->undo); free(x->redo); free(x->buf); free(x->lines); free(x->lst); free(x);
}

/* the one place the text changes: replaces [pos, pos+ndel) by ins */
static void raw_replace(struct doc *x, int pos, int ndel, const char *ins, int nins) {
    if (x->len - ndel + nins + 1 > x->cap) { x->cap = (x->len - ndel + nins + 1) * 2; x->buf = realloc(x->buf, x->cap); }
    memmove(x->buf + pos + nins, x->buf + pos + ndel, x->len - pos - ndel);
    memcpy(x->buf + pos, ins, nins);
    x->len += nins - ndel;
    x->buf[x->len] = 0;
    x->modified = x->dirty = true;
    x->ver++;
}

static void push_edit(struct edit **arr, int *n, int *cap, struct edit e) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *arr = realloc(*arr, *cap * sizeof **arr); }
    (*arr)[(*n)++] = e;
}

/* recorded edit; the caret lands after the inserted text */
static void rec(int pos, int ndel, const char *ins, int nins) {
    free_edits(d->redo, d->nredo);
    d->nredo = 0;
    struct edit e = { pos, ndel, nins, d->grp, malloc(ndel + 1), malloc(nins + 1) };
    memcpy(e.del, d->buf + pos, ndel);
    memcpy(e.ins, ins, nins);
    if (d->nundo >= 4000) { free_edits(d->undo, 100); memmove(d->undo, d->undo + 100, (d->nundo - 100) * sizeof *d->undo); d->nundo -= 100; }
    push_edit(&d->undo, &d->nundo, &d->ucap, e);
    raw_replace(d, pos, ndel, ins, nins);
    d->cur = d->anchor = pos + nins;
}

static void undo_redo(bool redo) {
    struct edit **from = redo ? &d->redo : &d->undo, **to = redo ? &d->undo : &d->redo;
    int *nf = redo ? &d->nredo : &d->nundo, *nt = redo ? &d->nundo : &d->nredo, *ct = redo ? &d->ucap : &d->rcap;
    if (!*nf) return;
    int g = (*from)[*nf - 1].grp;
    while (*nf && (*from)[*nf - 1].grp == g) {
        struct edit e = (*from)[--*nf];
        if (redo) { raw_replace(d, e.pos, e.ndel, e.ins, e.nins); d->cur = e.pos + e.nins; }
        else { raw_replace(d, e.pos, e.nins, e.del, e.ndel); d->cur = e.pos + e.ndel; }
        push_edit(to, nt, ct, e);
    }
    d->anchor = d->cur;
    d->lastop = 0;
}

static void begin_op(int op) {                     /* start a new undo group unless this repeats the previous op */
    if (op == 0 || d->lastop != op) d->grp = ++grp_ctr;
    d->lastop = op;
}

static void sel_range(int *lo, int *hi) { *lo = d->cur < d->anchor ? d->cur : d->anchor; *hi = d->cur < d->anchor ? d->anchor : d->cur; }

static void insert_text(const char *s, int n) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (d->len - (hi - lo) + n > MAX_FILE) return;
    rec(lo, hi - lo, s, n);
}

static void move_to(int off, bool extend, bool keep_col) {
    if (off < 0) off = 0;
    if (off > d->len) off = d->len;
    d->cur = off;
    if (!extend) d->anchor = d->cur;
    if (!keep_col) d->want_col = -1;
    d->lastop = 0;
}

/* ---------------------------------------------------------------- status line / dialogs state */
static char msg[120];
static uint32_t msg_until;
static bool msg_bad;
static void say(bool bad, const char *fmt, const char *arg) {
    snprintf(msg, sizeof msg, fmt, arg);
    msg_bad = bad;
    msg_until = uptime_ms() + 4500;
}

enum { M_NONE, M_OPEN, M_SAVEAS, M_NEWFILE, M_NEWDIR, M_OPENDIR, M_GOTO, M_CLOSETAB, M_QUIT };
static int mode;
static char prompt[PATH_MAX];
static int pending_close = -1;                      /* tab being closed in M_CLOSETAB */

static char root[PATH_MAX] = "/home/projects";

/* ---------------------------------------------------------------- file I/O */
static bool doc_load(struct doc *x, const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { say(true, "Cannot open %s", p); return false; }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); say(true, "%s is a folder", p); return false; }
    x->len = 0;
    for (;;) {
        if (x->len + 4096 + 1 > x->cap) { x->cap = x->cap * 2 + 4096; x->buf = realloc(x->buf, x->cap); }
        int n = read(fd, x->buf + x->len, 4096);
        if (n <= 0) break;
        x->len += n;
        if (x->len >= MAX_FILE) { say(true, "%s", "File truncated at 1 MiB"); break; }
    }
    close(fd);
    for (int i = 0; i < x->len; i++) if (x->buf[i] == '\r') { memmove(x->buf + i, x->buf + i + 1, x->len - i - 1); x->len--; i--; }
    x->buf[x->len] = 0;
    x->cur = x->anchor = 0;
    x->modified = false;
    x->dirty = true;
    return true;
}

static bool doc_save(struct doc *x, const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { say(true, "Cannot write %s", p); return false; }
    int off = 0;
    while (off < x->len) {
        int n = write(fd, x->buf + off, x->len - off);
        if (n <= 0) { close(fd); say(true, "Write failed: %s", p); return false; }
        off += n;
    }
    close(fd);
    if (p != x->path) snprintf(x->path, sizeof x->path, "%s", p);
    x->lang = lang_of(x->path);
    x->dirty = true;
    x->modified = false;
    return true;
}

static int find_doc(const char *p) {
    for (int i = 0; i < ndocs; i++) if (!strcmp(docs[i]->path, p)) return i;
    return -1;
}

static void select_doc(int i) { curdoc = i; d = i >= 0 && i < ndocs ? docs[i] : NULL; }

static struct doc *open_file(const char *p) {
    int i = find_doc(p);
    if (i >= 0) { select_doc(i); return d; }
    if (ndocs == MAX_TABS) { say(true, "%s", "Too many tabs open"); return NULL; }
    struct doc *x = doc_new(p);
    if (!doc_load(x, p)) { doc_free(x); return NULL; }
    docs[ndocs++] = x;
    select_doc(ndocs - 1);
    return x;
}

static void close_doc(int i) {
    doc_free(docs[i]);
    memmove(docs + i, docs + i + 1, (ndocs - i - 1) * sizeof *docs);
    ndocs--;
    select_doc(curdoc >= ndocs ? ndocs - 1 : (curdoc > i ? curdoc - 1 : curdoc));
}

static void save_all(void) {
    for (int i = 0; i < ndocs; i++) if (docs[i]->modified && docs[i]->path[0]) doc_save(docs[i], docs[i]->path);
}

static bool any_modified(void) {
    for (int i = 0; i < ndocs; i++) if (docs[i]->modified) return true;
    return false;
}

/* ---------------------------------------------------------------- project tree */
struct trow { char *path; char name[64]; bool dir; int depth; };
static struct trow *rows;
static int nrows, rcap_rows;
static char *open_dirs[64];
static int nopen_dirs;
static int tree_scroll;
static uint32_t tree_stamp;

static bool dir_is_open(const char *p) {
    for (int i = 0; i < nopen_dirs; i++) if (!strcmp(open_dirs[i], p)) return true;
    return false;
}
static void dir_toggle(const char *p) {
    for (int i = 0; i < nopen_dirs; i++)
        if (!strcmp(open_dirs[i], p)) { free(open_dirs[i]); open_dirs[i] = open_dirs[--nopen_dirs]; return; }
    if (nopen_dirs < 64) open_dirs[nopen_dirs++] = strdup(p);
}

struct tent { char name[64]; bool dir; };
static int cmp_tent(const void *a, const void *b) {
    const struct tent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void scan(const char *dir, int depth) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct tent *es = malloc(256 * sizeof *es);
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 256) {
        if (de->d_name[0] == '.') continue;
        snprintf(es[n].name, sizeof es[n].name, "%s", de->d_name);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        es[n].dir = de->d_type == DT_DIR;
        if (stat(p, &st) == 0) es[n].dir = S_ISDIR(st.st_mode);
        n++;
    }
    closedir(dp);
    qsort(es, n, sizeof *es, cmp_tent);
    for (int i = 0; i < n && depth < 8; i++) {
        if (nrows == rcap_rows) { rcap_rows = rcap_rows ? rcap_rows * 2 : 128; rows = realloc(rows, rcap_rows * sizeof *rows); }
        struct trow *r = &rows[nrows++];
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, es[i].name);
        r->path = strdup(p);
        snprintf(r->name, sizeof r->name, "%s", es[i].name);
        r->dir = es[i].dir; r->depth = depth;
        if (r->dir && dir_is_open(p)) scan(p, depth + 1);
    }
    free(es);
}

static void tree_refresh(void) {
    for (int i = 0; i < nrows; i++) free(rows[i].path);
    nrows = 0;
    scan(root, 0);
    tree_stamp = uptime_ms();
}

static void set_root(const char *p) {
    struct stat st;
    if (stat(p, &st) || !S_ISDIR(st.st_mode)) { say(true, "%s is not a folder", p); return; }
    snprintf(root, sizeof root, "%s", p);
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') root[--n] = 0;
    for (int i = 0; i < nopen_dirs; i++) free(open_dirs[i]);
    nopen_dirs = 0; tree_scroll = 0;
    tree_refresh();
}

/* ---------------------------------------------------------------- build / run with captured output */
static char *out;
static int outlen, outcap;
static int *olines, nolines, olcap;
static int out_top;
static bool out_stick = true, out_open = true;
static int out_h = 190;
static int proc_pid = -1, proc_fd = -1, proc_in = -1, proc_kind;     /* kind: 1 build, 2 run */
static bool run_after_build;
static char proc_dir[PATH_MAX], last_exe[PATH_MAX], last_dir[PATH_MAX], run_target[PATH_MAX];
static int run_lang;
static char status_txt[64];
static color_t status_col;
static uint32_t proc_t0;
static char stdin_line[160];

static struct { char path[PATH_MAX]; int line; } marks[MAX_MARKS];
static int nmarks;

static void out_append(const char *s, int n) {
    if (!out) { outcap = OUT_MAX + 4096; out = malloc(outcap); }
    if (outlen + n > OUT_MAX) {                       /* drop the oldest half, at a line boundary */
        int cut = OUT_MAX / 2;
        while (cut < outlen && out[cut - 1] != '\n') cut++;
        memmove(out, out + cut, outlen - cut);
        outlen -= cut;
    }
    if (n > OUT_MAX) { s += n - OUT_MAX; n = OUT_MAX; }
    memcpy(out + outlen, s, n);
    outlen += n;
    for (int i = 0; i < n; i++) if (out[outlen - n + i] == '\r') out[outlen - n + i] = ' ';
}
static void out_puts(const char *s) { out_append(s, strlen(s)); }

static void out_index(void) {
    int n = 1;
    for (int i = 0; i < outlen; i++) if (out[i] == '\n') n++;
    if (n > olcap) { olcap = n + 256; olines = realloc(olines, olcap * sizeof *olines); }
    int k = 0;
    olines[k++] = 0;
    for (int i = 0; i < outlen; i++) if (out[i] == '\n') olines[k++] = i + 1;
    nolines = n;
    if (nolines > 1 && out[outlen - 1] == '\n') nolines--;          /* no phantom empty last line */
    if (!outlen) nolines = 0;
}

/* "dir/file.c:12: error: ..." or "lua: file.lua:3: ..." -> resolved path and line; false if the line isn't a location */
static bool parse_loc(const char *s, int n, char *path, int *line) {
    for (int p = 1; p + 1 < n; p++) {
        if (s[p] != ':' || !isdigit((unsigned char)s[p + 1])) continue;
        int q = p + 1, v = 0;
        while (q < n && isdigit((unsigned char)s[q])) v = v * 10 + s[q++] - '0';
        if (q < n && s[q] != ':' && s[q] != ' ' && s[q] != ')') continue;
        int a = p;
        while (a > 0 && s[a - 1] != ' ' && s[a - 1] != '(' && s[a - 1] != '\t') a--;
        if (a == p) continue;
        char tok[PATH_MAX];
        int tl = p - a;
        if (tl >= (int)sizeof tok) continue;
        memcpy(tok, s + a, tl); tok[tl] = 0;
        if (!strchr(tok, '.')) continue;
        const char *t = tok;
        while (t[0] == '.' && t[1] == '/') t += 2;
        if (t[0] == '/') snprintf(path, PATH_MAX, "%s", t);
        else snprintf(path, PATH_MAX, "%s/%s", proc_dir[0] ? proc_dir : root, t);
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        *line = v;
        return true;
    }
    return false;
}

static void collect_marks(void) {                     /* error lines in the output -> gutter marks */
    nmarks = 0;
    out_index();
    for (int l = 0; l < nolines && nmarks < MAX_MARKS; l++) {
        int s = olines[l], e = l + 1 < nolines ? olines[l + 1] - 1 : outlen;
        char path[PATH_MAX];
        int ln;
        if (parse_loc(out + s, e - s, path, &ln) && !has(out + s, e - s, "warning")) {
            snprintf(marks[nmarks].path, PATH_MAX, "%s", path);
            marks[nmarks++].line = ln;
        }
    }
}

static void set_status(const char *t, color_t c) { snprintf(status_txt, sizeof status_txt, "%s", t); status_col = c; }

static bool start_proc(int kind, const char *exe, char *const argv[], const char *cwd) {
    int ofd[2], ifd[2] = { -1, -1 };
    if (pipe(ofd) < 0) { say(true, "%s", "pipe failed"); return false; }
    int in = -1;
    if (kind == 2 && pipe(ifd) == 0) in = ifd[0];
    else in = open("/dev/null", O_RDONLY);
    struct spawn_opts o = { { in, ofd[1], ofd[1] }, cwd, 0 };
    int pid = spawnve(exe, argv, environ, &o);
    close(ofd[1]);
    if (kind == 2 && ifd[0] >= 0) close(ifd[0]); else if (in >= 0) close(in);
    if (pid < 0) {
        close(ofd[0]);
        if (ifd[1] >= 0) close(ifd[1]);
        out_puts("cannot start "); out_puts(exe); out_puts("\n");
        set_status("could not start", UI_BAD);
        return false;
    }
    proc_pid = pid; proc_fd = ofd[0]; proc_in = ifd[1]; proc_kind = kind; proc_t0 = uptime_ms();
    snprintf(proc_dir, sizeof proc_dir, "%s", cwd);
    set_status(kind == 1 ? "building..." : "running...", UI_ACCENT);
    return true;
}

static void stop_proc(void) {
    if (proc_pid > 0) { kill(proc_pid, SIGKILL); out_puts("[stopped]\n"); }
}

static void run_target_now(void) {
    out_puts("$ "); out_puts(run_lang == L_LUA ? "lua " : ""); out_puts(run_target); out_puts("\n");
    if (run_lang == L_LUA) {
        char *argv[] = { "lua", run_target, NULL };
        start_proc(2, "/bin/lua", argv, last_dir);
    } else {
        char *argv[] = { run_target, NULL };
        start_proc(2, run_target, argv, last_dir);
    }
}

static void finish_proc(int status) {
    char t[64];
    int kind = proc_kind;
    proc_pid = -1; proc_kind = 0;
    if (proc_in >= 0) { close(proc_in); proc_in = -1; }
    if (status == 128 + SIGKILL) { set_status("stopped", UI_WARN); return; }
    uint32_t ms = uptime_ms() - proc_t0;
    snprintf(t, sizeof t, "[%s exited with code %d, %u.%02us]\n", kind == 1 ? "build" : "program", status, ms / 1000, (ms % 1000) / 10);
    if (outlen && out[outlen - 1] != '\n') out_puts("\n");
    out_puts(t);
    if (kind == 1) {
        collect_marks();
        if (status == 0) {
            set_status("build ok", UI_GOOD);
            if (run_after_build) run_target_now();
        } else {
            snprintf(t, sizeof t, "build failed (%d error%s)", nmarks, nmarks == 1 ? "" : "s");
            set_status(t, UI_BAD);
        }
    } else {
        collect_marks();
        snprintf(t, sizeof t, status ? "exit code %d" : "finished (exit %d)", status);
        set_status(t, status ? UI_WARN : UI_GOOD);
    }
    run_after_build = false;
    tree_refresh();
}

static void pump_proc(void) {
    if (proc_pid <= 0) return;
    char b[2048];
    for (int k = 0; k < 8 && proc_fd >= 0; k++) {
        struct pollfd p = { proc_fd, POLLIN, 0 };
        if (poll(&p, 1, 0) <= 0) break;
        int n = read(proc_fd, b, sizeof b);
        if (n > 0) out_append(b, n);
        else { close(proc_fd); proc_fd = -1; }
    }
    if (proc_fd < 0) {
        int st = 0;
        if (waitpid(proc_pid, &st, WNOHANG) == proc_pid) finish_proc(WEXITSTATUS(st));
    }
}

static void dir_of(const char *path, char *dir) {
    snprintf(dir, PATH_MAX, "%s", path);
    char *s = strrchr(dir, '/');
    if (s && s != dir) *s = 0; else if (s) s[1] = 0;
}

static bool file_mentions(const char *p, const char *needle) {   /* does the file contain this text (for -lgui detection) */
    int fd = open(p, O_RDONLY);
    if (fd < 0) return false;
    static char b[MAX_FILE + 1];
    int n = read(fd, b, MAX_FILE);
    close(fd);
    if (n <= 0) return false;
    b[n] = 0;
    return strstr(b, needle) != NULL;
}

static bool has_makefile(const char *dir) {
    static const char *const n[] = { "GNUmakefile", "makefile", "Makefile" };
    for (int i = 0; i < 3; i++) {
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, n[i]);
        if (stat(p, &st) == 0) return true;
    }
    return false;
}

/* Ctrl+B / F5. With a Makefile next to the file, /bin/make builds the project (F5 then runs ./<folder name>). Otherwise a .c file directly in the project folder is its own program; inside a subfolder, every .c file there
 * is built together (sieve/main.c + sieve.c). The result goes to /tmp. */
static void build(bool and_run) {
    if (proc_pid > 0) { say(true, "%s", "Already running - press Stop first"); return; }
    if (!d) { say(true, "%s", "Open a .c or .lua file first"); return; }
    if (d->lang == L_LUA) {
        if (!d->path[0]) { mode = M_SAVEAS; snprintf(prompt, sizeof prompt, "%s/untitled.lua", root); return; }
        if (!and_run) { say(false, "%s", "Lua needs no build - press F5 to run"); return; }
        save_all();
        dir_of(d->path, last_dir);
        snprintf(run_target, sizeof run_target, "%s", d->path);
        run_lang = L_LUA;
        out_open = true;
        outlen = 0; nmarks = 0; out_top = 0; out_stick = true;
        run_target_now();
        return;
    }
    if (d->lang != L_C) { say(true, "%s", "Build works on .c files; F5 also runs .lua"); return; }
    if (!d->path[0]) { mode = M_SAVEAS; snprintf(prompt, sizeof prompt, "%s/untitled.c", root); return; }
    save_all();
    char dir[PATH_MAX], names[32][96];
    int nf = 0;
    dir_of(d->path, dir);
    if (has_makefile(dir)) {
        snprintf(last_dir, sizeof last_dir, "%s", dir);
        snprintf(last_exe, sizeof last_exe, "%s/%s", dir, base(dir));
        snprintf(run_target, sizeof run_target, "%s", last_exe);
        run_lang = L_C;
        out_open = true;
        outlen = 0; nmarks = 0; out_top = 0; out_stick = true;
        out_puts("$ make\n");
        run_after_build = and_run;
        char *mv[] = { "make", NULL };
        start_proc(1, "/bin/make", mv, dir);
        return;
    }
    size_t bl = strlen(base(d->path));
    if (!strcmp(dir, root)) {
        if (!strcmp(base(d->path) + bl - 2, ".h")) { say(true, "%s", "Open a .c file to build"); return; }
        snprintf(names[nf++], 96, "%s", base(d->path));
    } else {
        DIR *dp = opendir(dir);
        struct dirent *de;
        while (dp && (de = readdir(dp)) && nf < 32) {
            size_t l = strlen(de->d_name);
            if (l > 2 && !strcmp(de->d_name + l - 2, ".c") && l < 96) snprintf(names[nf++], 96, "%s", de->d_name);
        }
        if (dp) closedir(dp);
    }
    if (!nf) { say(true, "%s", "No .c files here"); return; }
    char stem[96];
    snprintf(stem, sizeof stem, "%s", nf == 1 && !strcmp(dir, root) ? names[0] : base(dir));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    snprintf(last_exe, sizeof last_exe, "/tmp/%s", stem);
    snprintf(last_dir, sizeof last_dir, "%s", dir);
    snprintf(run_target, sizeof run_target, "%s", last_exe);
    run_lang = L_C;
    char *argv[48];
    int ac = 0;
    argv[ac++] = "tcc";
    bool gui = false;
    for (int i = 0; i < nf; i++) {
        argv[ac++] = names[i];
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (file_mentions(p, "ui.h") || file_mentions(p, "image.h")) gui = true;
    }
    argv[ac++] = "-o"; argv[ac++] = last_exe;
    if (gui) argv[ac++] = "-lgui";
    argv[ac] = NULL;
    out_open = true;
    outlen = 0; nmarks = 0; out_top = 0; out_stick = true;
    char cmd[400] = "$";
    for (int i = 0; argv[i] && strlen(cmd) < 360; i++) { strcat(cmd, " "); strcat(cmd, argv[i]); }
    strcat(cmd, "\n");
    out_puts(cmd);
    run_after_build = and_run;
    start_proc(1, "/bin/tcc", argv, dir);
}

/* ---------------------------------------------------------------- find / replace */
static bool find_open, replace_open, find_case;
static char fq[128], rq[128];
static unsigned fc_ver = ~0u; static int fc_total, fc_index; static char fc_q[128]; static bool fc_case; static struct doc *fc_doc;

static bool match_at(int pos, int n) {
    for (int i = 0; i < n; i++) {
        int a = (unsigned char)d->buf[pos + i], b = (unsigned char)fq[i];
        if (find_case ? a != b : tolower(a) != tolower(b)) return false;
    }
    return true;
}

static void find_next(bool backward) {
    int n = strlen(fq);
    if (!n || !d) return;
    int lo, hi;
    sel_range(&lo, &hi);
    int start = backward ? lo - 1 : hi;
    for (int k = 0; k <= d->len; k++) {
        int pos = backward ? ((start - k) % (d->len + 1) + d->len + 1) % (d->len + 1) : (start + k) % (d->len + 1);
        if (pos + n <= d->len && match_at(pos, n)) { d->anchor = pos; d->cur = pos + n; d->want_col = -1; d->lastop = 0; return; }
    }
    say(true, "No matches for \"%s\"", fq);
}

static void find_stats(void) {            /* cached "3 of 12" */
    if (!d) return;
    int n = strlen(fq);
    if (fc_doc == d && fc_ver == d->ver && fc_case == find_case && !strcmp(fc_q, fq) && fc_index >= -1) {
        int lo, hi;                       /* only the selection can have moved */
        sel_range(&lo, &hi);
        fc_index = 0;
        if (n && hi - lo == n && match_at(lo, n)) {
            int k = 0;
            for (int p = 0; p + n <= lo; p++) if (match_at(p, n)) k++;
            fc_index = k + 1;
        }
        return;
    }
    fc_doc = d; fc_ver = d->ver; fc_case = find_case; snprintf(fc_q, sizeof fc_q, "%s", fq);
    fc_total = 0;
    for (int p = 0; n && p + n <= d->len; p++) if (match_at(p, n)) fc_total++;
}

static void replace_one(void) {
    int n = strlen(fq), lo, hi;
    if (!d || !n) return;
    sel_range(&lo, &hi);
    if (hi - lo == n && match_at(lo, n)) { begin_op(0); rec(lo, n, rq, strlen(rq)); }
    find_next(false);
}

static void replace_all(void) {
    int n = strlen(fq), cnt = 0;
    if (!d || !n) return;
    int *pos = malloc((d->len / n + 1) * sizeof *pos);
    for (int p = 0; p + n <= d->len; ) if (match_at(p, n)) { pos[cnt++] = p; p += n; } else p++;
    begin_op(0);
    for (int i = cnt - 1; i >= 0; i--) rec(pos[i], n, rq, strlen(rq));       /* back to front keeps offsets valid */
    free(pos);
    char t[24];
    snprintf(t, sizeof t, "%d", cnt);
    say(false, "Replaced %s occurrence(s)", t);
}

/* ---------------------------------------------------------------- editing commands */
static void line_range_of_sel(int *l0, int *l1) {
    int lo, hi;
    sel_range(&lo, &hi);
    *l0 = line_of(d, lo);
    *l1 = line_of(d, hi > lo && d->buf[hi - 1] == '\n' ? hi - 1 : hi);
}

static void indent_lines(bool out_dent) {
    int l0, l1;
    int lo, hi;
    sel_range(&lo, &hi);
    line_range_of_sel(&l0, &l1);
    int shift_cur = 0, shift_anc = 0;
    begin_op(0);
    int ocur = d->cur, oanc = d->anchor;
    for (int l = l1; l >= l0; l--) {
        int s = d->lines[l], rm = 0;
        if (!out_dent) {
            if (line_end(d, l) == s) continue;                       /* leave blank lines alone */
            rec(s, 0, "    ", TAB);
            if (s <= ocur) shift_cur += TAB;
            if (s <= oanc) shift_anc += TAB;
        } else {
            while (rm < TAB && s + rm < line_end(d, l) && d->buf[s + rm] == ' ') rm++;
            if (!rm && s < line_end(d, l) && d->buf[s] == '\t') rm = 1;
            if (!rm) continue;
            rec(s, rm, "", 0);
            if (s < ocur) shift_cur -= rm < ocur - s ? rm : ocur - s;
            if (s < oanc) shift_anc -= rm < oanc - s ? rm : oanc - s;
        }
        reindex(d);
    }
    d->cur = ocur + shift_cur; d->anchor = oanc + shift_anc;
}

static void toggle_comment(void) {
    if (d->lang == L_TEXT) return;
    const char *pfx = d->lang == L_C ? "// " : "-- ";
    int pl = 3, l0, l1;
    line_range_of_sel(&l0, &l1);
    bool all = true;
    for (int l = l0; l <= l1; l++) {
        int s = d->lines[l], e = line_end(d, l);
        while (s < e && (d->buf[s] == ' ' || d->buf[s] == '\t')) s++;
        if (s < e && strncmp(d->buf + s, pfx, 2)) all = false;
    }
    int ocur = d->cur, oanc = d->anchor;
    begin_op(0);
    for (int l = l1; l >= l0; l--) {
        int s = d->lines[l], e = line_end(d, l);
        while (s < e && (d->buf[s] == ' ' || d->buf[s] == '\t')) s++;
        if (s == e) continue;
        int delta;
        if (all) { int rm = d->buf[s + 2] == ' ' ? 3 : 2; rec(s, rm, "", 0); delta = -rm; }
        else { rec(s, 0, pfx, pl); delta = pl; }
        if (s <= ocur) ocur += delta;
        if (s <= oanc) oanc += delta;
        reindex(d);
    }
    d->cur = ocur; d->anchor = oanc;
}

static bool lua_opens(const char *s, int n) {      /* does this Lua line open a block */
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n && (s[n - 1] == '{' || (s[n - 1] == ')' && has(s, n, "function")))) return true;
    int e = n;
    while (n > 0 && idchar(s[n - 1])) n--;
    int w = e - n;
    return (w == 4 && !memcmp(s + n, "then", 4)) || (w == 2 && !memcmp(s + n, "do", 2)) || (w == 4 && !memcmp(s + n, "else", 4)) ||
           (w == 6 && !memcmp(s + n, "repeat", 6));
}

static void dedent_typed(void) {                   /* after typing } or end/else/until on an otherwise empty line */
    int l = line_of(d, d->cur), s = d->lines[l], i = s;
    while (i < d->cur && d->buf[i] == ' ') i++;
    int ws = i - s;
    if (ws < TAB) return;
    const char *w = d->buf + i;
    int wl = d->cur - i;
    bool ok = d->lang == L_C ? (wl == 1 && w[0] == '}') :
              (wl == 3 && !memcmp(w, "end", 3)) || (wl == 4 && !memcmp(w, "else", 4)) || (wl == 5 && !memcmp(w, "until", 5)) ||
              (wl == 6 && !memcmp(w, "elseif", 6));
    if (!ok) return;
    int keep = d->lastop;
    int cur = d->cur;
    d->grp = d->grp;                               /* same undo group as the typed character */
    rec(s, TAB, "", 0);
    d->cur = d->anchor = cur - TAB;
    d->lastop = keep;
}

static void enter_key(void) {
    int lo, hi;
    sel_range(&lo, &hi);
    int ls = d->lines[line_of(d, lo)], ni = 0;
    char ind[64];
    while (ls + ni < lo && ni < 60 && (d->buf[ls + ni] == ' ' || d->buf[ls + ni] == '\t')) { ind[ni] = d->buf[ls + ni]; ni++; }
    int p = lo - 1;
    while (p >= ls && (d->buf[p] == ' ' || d->buf[p] == '\t')) p--;
    bool extra = false, between = false;
    if (d->lang == L_C && p >= ls && d->buf[p] == '{') {
        extra = true;
        int q = hi;
        while (q < d->len && (d->buf[q] == ' ' || d->buf[q] == '\t')) q++;
        between = q < d->len && d->buf[q] == '}';
    } else if (d->lang == L_LUA && lua_opens(d->buf + ls, p - ls + 1)) extra = true;
    char ins[160];
    int n = 0;
    ins[n++] = '\n';
    memcpy(ins + n, ind, ni); n += ni;
    if (extra) { memset(ins + n, ' ', TAB); n += TAB; }
    int back = 0;
    if (between) { ins[n++] = '\n'; memcpy(ins + n, ind, ni); n += ni; back = ni + 1; }
    begin_op(0);
    rec(lo, hi - lo, ins, n);
    d->cur = d->anchor = lo + n - back;
}

static void del_sel_or(int dir) {                    /* backspace (-1) and delete (+1) */
    int lo, hi;
    sel_range(&lo, &hi);
    begin_op(dir < 0 ? 2 : 3);
    if (hi > lo) { begin_op(0); rec(lo, hi - lo, "", 0); }
    else if (dir < 0 && d->cur > 0) {
        int n = 1;                                   /* backspace over a full indent unit of spaces */
        int ls = d->lines[line_of(d, d->cur)];
        bool blank = true;
        for (int i = ls; i < d->cur; i++) if (d->buf[i] != ' ') blank = false;
        if (blank && d->cur - ls >= TAB && (d->cur - ls) % TAB == 0) n = TAB;
        rec(d->cur - n, n, "", 0);
    } else if (dir > 0 && d->cur < d->len) rec(d->cur, 1, "", 0);
    d->want_col = -1;
}

static void copy_sel(void) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (hi > lo) gui_clip_set(d->buf + lo, hi - lo);
}

static void handle_key(const struct gui_event *k, int vis) {
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    int l = line_of(d, d->cur);
    if (ctrl) {
        int key = k->key < 128 ? tolower(k->key) : k->key;
        int lo, hi;
        switch (key) {
        case 'z': begin_op(0); undo_redo(shift); break;
        case 'y': undo_redo(true); break;
        case 'a': d->anchor = 0; d->cur = d->len; break;
        case 'c': copy_sel(); break;
        case 'x': copy_sel(); sel_range(&lo, &hi); if (hi > lo) { begin_op(0); rec(lo, hi - lo, "", 0); } break;
        case 'v': { int n; char *c = gui_clip_get(&n); if (c) { begin_op(0); insert_text(c, n); } free(c); break; }
        case '/': toggle_comment(); break;
        case 'd': {                                  /* duplicate the current line */
            int s = d->lines[l], e = line_end(d, l), cur = d->cur;
            char *t = malloc(e - s + 2);
            t[0] = '\n'; memcpy(t + 1, d->buf + s, e - s);
            begin_op(0); rec(e, 0, t, e - s + 1); d->cur = d->anchor = cur + e - s + 1;
            free(t);
            break;
        }
        case KEY_LEFT: { int i = d->cur; while (i > 0 && !isword(d->buf[i - 1])) i--; while (i > 0 && isword(d->buf[i - 1])) i--; move_to(i, shift, false); break; }
        case KEY_RIGHT: { int i = d->cur; while (i < d->len && !isword(d->buf[i])) i++; while (i < d->len && isword(d->buf[i])) i++; move_to(i, shift, false); break; }
        case KEY_HOME: move_to(0, shift, false); break;
        case KEY_END: move_to(d->len, shift, false); break;
        }
        return;
    }
    switch (k->key) {
    case KEY_LEFT:
        if (d->cur != d->anchor && !shift) move_to(d->cur < d->anchor ? d->cur : d->anchor, false, false);
        else move_to(d->cur - 1, shift, false);
        return;
    case KEY_RIGHT:
        if (d->cur != d->anchor && !shift) move_to(d->cur > d->anchor ? d->cur : d->anchor, false, false);
        else move_to(d->cur + 1, shift, false);
        return;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int dl = k->key == KEY_UP ? -1 : k->key == KEY_DOWN ? 1 : k->key == KEY_PGUP ? -vis : vis;
        if (d->want_col < 0) d->want_col = vcol(d, l, d->cur);
        int nl = l + dl;
        if (nl < 0) { move_to(0, shift, false); return; }
        if (nl >= d->nlines) { move_to(d->len, shift, false); return; }
        move_to(off_at_vcol(d, nl, d->want_col), shift, true);
        return;
    }
    case KEY_HOME: {
        int s = d->lines[l], i = s;
        while (i < line_end(d, l) && (d->buf[i] == ' ' || d->buf[i] == '\t')) i++;
        move_to(d->cur == i ? s : i, shift, false);
        return;
    }
    case KEY_END: move_to(line_end(d, l), shift, false); return;
    case KEY_BACKSPACE: del_sel_or(-1); return;
    case KEY_DELETE: del_sel_or(1); return;
    case KEY_ENTER: enter_key(); d->want_col = -1; return;
    case KEY_TAB: {
        int l0, l1;
        line_range_of_sel(&l0, &l1);
        if (shift || l1 > l0) { indent_lines(shift); return; }
        int sp = TAB - vcol(d, l, d->cur) % TAB;
        begin_op(0);
        insert_text("    ", sp);
        return;
    }
    case KEY_ESC: d->anchor = d->cur; return;
    }
    if (k->ch >= 32 && k->ch < 127 && !(k->mods & MOD_ALT)) {
        char ch = k->ch;
        begin_op(1);
        insert_text(&ch, 1);
        d->want_col = -1;
        if (ch == '}' || (d->lang == L_LUA && isalpha((unsigned char)ch))) dedent_typed();
    }
}

static void goto_line(struct doc *x, int line, int vis) {
    reindex(x);
    if (line < 1) line = 1;
    if (line > x->nlines) line = x->nlines;
    x->cur = x->anchor = x->lines[line - 1];
    x->top = line - 1 - vis / 2;
    if (x->top < 0) x->top = 0;
}

static int flash_line = -1;
static uint32_t flash_until;
static struct doc *flash_doc;

static void jump_to(const char *path, int line, int vis) {
    struct doc *x = open_file(path);
    if (!x) return;
    goto_line(x, line, vis);
    flash_line = line - 1; flash_until = uptime_ms() + 1600; flash_doc = x;
}

/* ---------------------------------------------------------------- 3-way dialog */
static int dlg3(struct ui *ui, const char *title, const char *text, const char *b1, const char *b2) {
    int W = ui->win->w, H = ui->win->h, pw = 440, ph = 138, px = (W - pw) / 2, py = (H - ph) / 2, r = 0;
    struct canvas *c = ui->c;
    gfx_fill(c, 0, 0, W, H, RGBA(0, 0, 0, 150));
    gfx_shadow(c, px, py, pw, ph, 12, 14, 120);
    gfx_round_fill(c, px, py, pw, ph, 12, UI_PANEL);
    gfx_round_rect(c, px, py, pw, ph, 12, RGBA(255, 255, 255, 40));
    gfx_text(c, &font_ui_bold, px + 20, py + 16, title, UI_TEXT);
    ui_text_fit(ui, &font_ui, px + 20, py + 44, pw - 40, text, UI_TEXT_DIM);
    if (ui_button(ui, px + pw - 20 - 100, py + ph - 50, 100, 34, b1, UI_ACCENT_BTN)) r = 1;
    if (ui_button(ui, px + pw - 20 - 224, py + ph - 50, 114, 34, b2, UI_NORMAL_BTN)) r = 2;
    if (ui_button(ui, px + 20, py + ph - 50, 90, 34, "Cancel", UI_GHOST_BTN)) r = -1;
    struct gui_event k;
    while (ui_key(ui, &k)) if (k.key == KEY_ESC) r = -1;
    return r;
}

/* ---------------------------------------------------------------- main */
static bool dragging;
static int out_drag;
static char last_title[128];

static int tab_width(int avail) {
    int tw = ndocs ? avail / ndocs : 180;
    return tw > 190 ? 190 : tw < 70 ? 70 : tw;
}

static void new_untitled(void) {
    if (ndocs == MAX_TABS) return;
    docs[ndocs++] = doc_new(NULL);
    select_doc(ndocs - 1);
}

static void request_close_tab(int i) {
    if (docs[i]->modified) { pending_close = i; mode = M_CLOSETAB; }
    else close_doc(i);
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Code", 1080, 690, 0);
    if (!w) return 1;
    CW = font_mono.g['M' - 32].adv;
    LH = font_mono.height + 5;
    struct ui ui;
    ui_init(&ui, w);
    bool sidebar = true;
    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) snprintf(root, sizeof root, "%s", argv[1]);
        else if (stat(argv[1], &st) == 0) { dir_of(argv[1], root); open_file(argv[1]); }
    }
    {
        struct stat st;
        if (stat(root, &st) || !S_ISDIR(st.st_mode)) snprintf(root, sizeof root, "/home");
    }
    tree_refresh();
    uint32_t last_act = uptime_ms();
    for (;;) {
        if (!ui_poll(&ui, proc_pid > 0 ? 30 : 250)) {
            if (!any_modified() || mode == M_QUIT) break;
            ui.closed = false;
            mode = M_QUIT;
        }
        uint32_t now = uptime_ms();
        pump_proc();
        if (!proc_pid && now - tree_stamp > 3000 && mode == M_NONE) tree_refresh();
        int W = w->w, H = w->h;
        bool modal = mode != M_NONE;
        int sbw = sidebar ? SIDE_W : 0;
        int mx0 = sbw, mw = W - sbw;
        int oh = out_open ? out_h : 0;                                     /* output pane incl. header */
        int ey = TB_H + TABS_H, eh = H - SB_H - ey - (out_open ? oh + 5 : 0);
        if (eh < 80) { eh = 80; }
        int ex = mx0, ew = mw;
        int gutter = 0;
        if (d) { reindex(d); for (int n = d->nlines; n; n /= 10) gutter += CW; gutter += 28; }
        int tx0 = ex + gutter + 6, vis = eh / LH;
        int oy = ey + eh + 5;                                              /* output pane top */
        int fbx = ex + ew - 440 - 18, fby = ey + 6, fbw = 440, fbh = replace_open ? 76 : 40;
        int fq_x = fbx + 8, fq_y = fby + 5, fq_w = 190, fq_h = 30;
        int rq_y = fby + 40;

        /* ---- keep modal dialogs from leaking input to what is behind them ---- */
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, sd = ui.dbl, srp = ui.rpressed;
        int nk = ui.nkeys, ssc = ui.scroll;
        struct gui_event saved_keys[32];
        memcpy(saved_keys, ui.keys, sizeof saved_keys);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.dbl = ui.rpressed = false; ui.nkeys = 0; ui.scroll = 0; }

        /* ---- global shortcuts (taken before any widget can swallow the key) ---- */
        {
            int keep = 0;
            struct gui_event ks[32];
            for (int i = 0; i < ui.nkeys; i++) {
                struct gui_event k = ui.keys[i];
                bool ctrl = k.mods & MOD_CTRL, shift = k.mods & MOD_SHIFT, used = true;
                int key = k.key < 128 ? tolower(k.key) : k.key;
                if (k.key == KEY_F5) { if (shift) stop_proc(); else build(true); }
                else if (k.key == KEY_F3) { if (d) find_next(shift); }
                else if (ctrl && key == 's') { if (d) { if (d->path[0]) { if (doc_save(d, d->path)) say(false, "Saved %s", base(d->path)); tree_refresh(); } else { mode = M_SAVEAS; snprintf(prompt, sizeof prompt, "%s/untitled.c", root); } } }
                else if (ctrl && key == 'b') build(false);
                else if (ctrl && key == 'n') new_untitled();
                else if (ctrl && key == 'o') { mode = M_OPEN; snprintf(prompt, sizeof prompt, "%s/", root); }
                else if (ctrl && key == 'w') { if (d) request_close_tab(curdoc); }
                else if (ctrl && (key == 'f' || key == 'h')) {
                    if (d) {
                        find_open = true; replace_open = key == 'h';
                        int lo, hi;
                        sel_range(&lo, &hi);
                        if (hi > lo && hi - lo < (int)sizeof fq && !memchr(d->buf + lo, '\n', hi - lo)) { memcpy(fq, d->buf + lo, hi - lo); fq[hi - lo] = 0; }
                        ui.focus = ui_id(fq_x, fq_y, fq_w, fq_h);
                    }
                }
                else if (ctrl && key == 'g') { if (d) { mode = M_GOTO; prompt[0] = 0; } }
                else if (ctrl && key == '\\') sidebar = !sidebar;
                else if (ctrl && key == 'j') out_open = !out_open;
                else if (ctrl && k.key == KEY_TAB) { if (ndocs > 1) select_doc((curdoc + (shift ? ndocs - 1 : 1)) % ndocs); }
                else if (ctrl && (k.key == KEY_PGDN || k.key == KEY_PGUP)) { if (ndocs > 1) select_doc((curdoc + (k.key == KEY_PGDN ? 1 : ndocs - 1)) % ndocs); }
                else if (k.key == KEY_ESC && find_open) { find_open = false; ui.focus = 0; }
                else used = false;
                if (!used) ks[keep++] = k;
            }
            memcpy(ui.keys, ks, keep * sizeof ks[0]);
            ui.nkeys = keep;
        }

        /* ---- editor keys, when no text box has the focus ---- */
        if (d && !modal && !ui.focus) {
            struct gui_event k;
            while (ui_key(&ui, &k)) {
                handle_key(&k, vis);
                reindex(d);
                last_act = now;
                int l = line_of(d, d->cur);
                if (l < d->top) d->top = l;
                if (l >= d->top + vis) d->top = l - vis + 1;
                int cx = vcol(d, l, d->cur) * CW, viewW = ew - gutter - 24;
                if (cx - d->hx > viewW - CW) d->hx = cx - viewW + CW * 4;
                if (cx < d->hx) d->hx = cx > CW * 4 ? cx - CW * 4 : 0;
            }
        }

        /* ---- mouse in the editor ---- */
        bool over_find = find_open && smx >= fbx && smx < fbx + fbw && smy >= fby && smy < fby + fbh;
        if (d && !modal) {
            reindex(d);
            bool in_ed = ui_hover(&ui, tx0, ey, ew - (tx0 - ex), eh) && !over_find;
            if (in_ed && ui.scroll) d->top += ui.scroll * 3;
            if (ui.pressed && in_ed) {
                int l = d->top + (ui.my - ey - 4) / LH;
                if (l < 0) l = 0;
                if (l >= d->nlines) l = d->nlines - 1;
                int vc = (ui.mx - tx0 + d->hx + CW / 2) / CW;
                int off = off_at_vcol(d, l, vc < 0 ? 0 : vc);
                if (ui.dbl) {
                    int a = off, b = off;
                    while (a > d->lines[l] && isword(d->buf[a - 1])) a--;
                    while (b < line_end(d, l) && isword(d->buf[b])) b++;
                    d->anchor = a; d->cur = b;
                } else d->cur = d->anchor = off;
                dragging = true; d->want_col = -1; d->lastop = 0; last_act = now;
            } else if (dragging && (ui.buttons & 1)) {
                int l = d->top + (ui.my - ey - 4) / LH;
                if (l < d->top) l = d->top - 1 < 0 ? 0 : d->top - 1;
                if (l >= d->nlines) l = d->nlines - 1;
                int vc = (ui.mx - tx0 + d->hx + CW / 2) / CW;
                d->cur = off_at_vcol(d, l, vc < 0 ? 0 : vc);
                if (ui.my > ey + eh && d->top < d->nlines - vis) d->top++;
                if (ui.my < ey && d->top > 0) d->top--;
                last_act = now;
            }
            if (ui.released || !(ui.buttons & 1)) dragging = false;
            if (d->top > d->nlines - vis) d->top = d->nlines - vis;
            if (d->top < 0) d->top = 0;
        }

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;

        /* toolbar */
        gfx_fill(c, 0, 0, W, TB_H, UI_PANEL);
        gfx_fill(c, 0, TB_H - 1, W, 1, UI_BORDER);
        {
            int bx = 8, by = 5, bh = 32;
            if (ui_tool(&ui, bx, by, 64, bh, "Files", sidebar)) sidebar = !sidebar;
            bx += 68;
            if (ui_tool(&ui, bx, by, 58, bh, "Save", false) && d) {
                if (d->path[0]) { if (doc_save(d, d->path)) say(false, "Saved %s", base(d->path)); tree_refresh(); }
                else { mode = M_SAVEAS; snprintf(prompt, sizeof prompt, "%s/untitled.c", root); }
            }
            bx += 70;
            gfx_fill(c, bx - 6, 10, 1, 22, UI_BORDER);
            if (ui_button(&ui, bx, by, 76, bh, "Build", UI_NORMAL_BTN)) build(false);
            bx += 84;
            if (ui_button(&ui, bx, by, 72, bh, "Run", UI_ACCENT_BTN)) build(true);
            bx += 80;
            if (ui_button(&ui, bx, by, 70, bh, "Stop", proc_pid > 0 ? UI_NORMAL_BTN : UI_GHOST_BTN) && proc_pid > 0) stop_proc();
            bx += 84;
            char hint[160];
            snprintf(hint, sizeof hint, "Ctrl+B build   F5 run   Ctrl+F find   Ctrl+G go to line");
            int hw = gfx_text_width(&font_ui, hint);
            if (W - bx - 20 > hw) gfx_text(c, &font_ui, W - 16 - hw, (TB_H - font_ui.height) / 2, hint, UI_TEXT_DIM);
        }

        /* sidebar */
        if (sidebar) {
            gfx_fill(c, 0, TB_H, sbw, H - TB_H - SB_H, C_GUTTER);
            gfx_fill(c, sbw - 1, TB_H, 1, H - TB_H - SB_H, UI_BORDER);
            const char *rb = base(root);
            char head[80];
            snprintf(head, sizeof head, "%s", root[1] ? rb : "/");
            for (char *p = head; *p; p++) *p = toupper((unsigned char)*p);
            ui_text_fit(&ui, &font_ui_bold, 14, TB_H + 10, sbw - 28, head, UI_TEXT_DIM);
            int ty = TB_H + 34;
            if (ui_tool(&ui, 8, ty, 66, 26, "+ File", false)) { mode = M_NEWFILE; snprintf(prompt, sizeof prompt, "%s/", root); }
            if (ui_tool(&ui, 76, ty, 66, 26, "+ Dir", false)) { mode = M_NEWDIR; snprintf(prompt, sizeof prompt, "%s/", root); }
            if (ui_tool(&ui, 144, ty, 72, 26, "Folder", false)) { mode = M_OPENDIR; snprintf(prompt, sizeof prompt, "%s", root); }
            int ly = ty + 34, lh = H - SB_H - ly, RH = 26, tvis = lh / RH;
            struct canvas sc = *c;
            canvas_clip(&sc, 0, ly, sbw - 1, lh);
            if (ui_hover(&ui, 0, ly, sbw, lh) && ui.scroll) tree_scroll += ui.scroll * 2;
            if (tree_scroll > nrows - tvis) tree_scroll = nrows - tvis;
            if (tree_scroll < 0) tree_scroll = 0;
            for (int i = tree_scroll; i < nrows && i < tree_scroll + tvis + 1; i++) {
                struct trow *r = &rows[i];
                int ry = ly + (i - tree_scroll) * RH, rx = 8 + r->depth * 14;
                bool active = !r->dir && d && !strcmp(d->path, r->path);
                bool hov = ui_hover(&ui, 0, ry, sbw - 1, RH) && ry + RH <= ly + lh;
                if (active) gfx_fill(&sc, 4, ry + 1, sbw - 9, RH - 2, RGBA(0xB3, 0x88, 0xFF, 70));
                else if (hov) gfx_fill(&sc, 4, ry + 1, sbw - 9, RH - 2, RGBA(255, 255, 255, 14));
                if (r->dir) {                                              /* chevron */
                    bool op = dir_is_open(r->path);
                    float cx = rx + 6, cy = ry + RH / 2;
                    if (op) { gfx_line_aa(&sc, cx - 3, cy - 2, cx, cy + 2, 1.6f, UI_TEXT_DIM); gfx_line_aa(&sc, cx, cy + 2, cx + 3, cy - 2, 1.6f, UI_TEXT_DIM); }
                    else { gfx_line_aa(&sc, cx - 2, cy - 3, cx + 2, cy, 1.6f, UI_TEXT_DIM); gfx_line_aa(&sc, cx + 2, cy, cx - 2, cy + 3, 1.6f, UI_TEXT_DIM); }
                } else {
                    int lg = lang_of(r->name);
                    color_t ic = lg == L_C ? RGB(0x5B, 0xD8, 0xF5) : lg == L_LUA ? RGB(0x8C, 0x9C, 0xFF) : UI_TEXT_DIM;
                    gfx_round_fill(&sc, rx + 2, ry + RH / 2 - 4, 8, 8, 3, ic);
                }
                ui_text_fit(&ui, &font_ui, rx + 18, ry + (RH - font_ui.height) / 2, sbw - rx - 26, r->name, r->dir ? UI_TEXT : active ? UI_TEXT : RGB(0xCF, 0xCB, 0xE8));
                if (hov && ui.pressed) {
                    if (r->dir) { dir_toggle(r->path); tree_refresh(); }
                    else {
                        char p[PATH_MAX];
                        snprintf(p, sizeof p, "%s", r->path);
                        open_file(p);
                    }
                    ui.pressed = false;
                    break;                                                 /* rows may have been rebuilt */
                }
            }
            ui_vscroll(&ui, sbw - 12, ly, lh, nrows, tvis, &tree_scroll);
        }

        /* tab strip */
        gfx_fill(c, mx0, TB_H, mw, TABS_H, UI_PANEL);
        gfx_fill(c, mx0, TB_H + TABS_H - 1, mw, 1, UI_BORDER);
        {
            int tw = tab_width(mw - 8), x = mx0 + 4, closing = -1, switch_to = -1;
            for (int i = 0; i < ndocs; i++, x += tw) {
                struct doc *t = docs[i];
                bool act = i == curdoc, hov = ui_hover(&ui, x, TB_H, tw, TABS_H);
                if (act) { gfx_fill(c, x, TB_H + 4, tw - 2, TABS_H - 4, C_EDIT); gfx_fill(c, x, TB_H + 4, tw - 2, 2, UI_ACCENT); }
                else if (hov) gfx_fill(c, x, TB_H + 4, tw - 2, TABS_H - 5, RGBA(255, 255, 255, 12));
                const char *nm = t->path[0] ? base(t->path) : "untitled";
                ui_text_fit(&ui, &font_ui, x + 12, TB_H + 4 + (TABS_H - 4 - font_ui.height) / 2, tw - 44, nm, act ? UI_TEXT : UI_TEXT_DIM);
                int cx = x + tw - 20, cy = TB_H + 4 + (TABS_H - 4) / 2;
                bool hx_ = ui_hover(&ui, cx - 8, cy - 9, 18, 18);
                if (t->modified && !hx_) gfx_circle_fill(c, cx + 1, cy, 4, UI_ACCENT2);
                else if (act || hov) {
                    if (hx_) gfx_round_fill(c, cx - 8, cy - 9, 18, 18, 5, RGBA(255, 255, 255, 30));
                    gfx_line_aa(c, cx - 3, cy - 3, cx + 4, cy + 4, 1.6f, UI_TEXT_DIM);
                    gfx_line_aa(c, cx + 4, cy - 3, cx - 3, cy + 4, 1.6f, UI_TEXT_DIM);
                }
                if (ui.pressed && hov) { if (hx_) closing = i; else switch_to = i; }
            }
            if (ui_tool(&ui, x + 2, TB_H + 5, 28, TABS_H - 7, "+", false)) new_untitled();
            if (closing >= 0) request_close_tab(closing);
            else if (switch_to >= 0) select_doc(switch_to);
        }

        /* editor */
        gfx_fill(c, ex, ey, ew, eh, C_EDIT);
        if (!d) {
            int cy = ey + eh / 2 - 90;
            gfx_text_center(c, &font_big, ex, cy, ew, "Code", UI_ACCENT);
            gfx_text_center(c, &font_ui, ex, cy + 72, ew, "Open a file from the sidebar, or start something new.", UI_TEXT_DIM);
            static const char *const tips[] = { "Ctrl+N   new file", "Ctrl+O   open file", "Ctrl+B   build", "F5       build and run", "Ctrl+F   find and replace" };
            for (int i = 0; i < 5; i++) gfx_text(c, &font_mono, ex + ew / 2 - 110, cy + 112 + i * 22, tips[i], UI_TEXT_DIM);
        } else {
            gfx_fill(c, ex, ey, gutter, eh, C_GUTTER);
            gfx_fill(c, ex + gutter, ey, 1, eh, UI_BORDER);
            struct canvas cc = *c, gc = *c;
            canvas_clip(&cc, tx0, ey, ew - (tx0 - ex) - 10, eh);
            canvas_clip(&gc, ex, ey, gutter, eh);
            int lo, hi;
            sel_range(&lo, &hi);
            int curl = line_of(d, d->cur), fqn = find_open ? strlen(fq) : 0;
            for (int i = 0; i < vis + 1 && d->top + i < d->nlines; i++) {
                int l = d->top + i, y = ey + 4 + i * LH;
                if (y > ey + eh) break;
                int s = d->lines[l], e = line_end(d, l);
                bool errl = false;
                for (int m = 0; m < nmarks; m++) if (marks[m].line == l + 1 && !strcmp(marks[m].path, d->path)) errl = true;
                if (errl) gfx_fill(&cc, tx0 - 4, y - 1, ew, LH, RGBA(0xFF, 0x5F, 0x7A, 34));
                if (flash_doc == d && l == flash_line && now < flash_until) gfx_fill(&cc, tx0 - 4, y - 1, ew, LH, RGBA(0xFF, 0xC2, 0x4B, 60));
                if (l == curl && lo == hi) gfx_fill(&cc, tx0 - 4, y - 1, ew, LH, RGBA(0xB3, 0x88, 0xFF, 14));
                char num[16];
                snprintf(num, sizeof num, "%d", l + 1);
                gfx_text(&gc, &font_mono, ex + gutter - 12 - gfx_text_width(&font_mono, num), y + 1, num, errl ? UI_BAD : l == curl ? UI_ACCENT : C_LINENO);
                if (errl) gfx_circle_fill(&gc, ex + 9, y + LH / 2 - 1, 3, UI_BAD);
                /* find matches, selection */
                if (fqn) for (int p = s; p + fqn <= e; p++) if (match_at(p, fqn)) {
                    int x0 = tx0 + vcol(d, l, p) * CW - d->hx;
                    gfx_fill(&cc, x0, y - 1, fqn * CW, LH, RGBA(0xFF, 0xC2, 0x4B, 70));
                    p += fqn - 1;
                }
                if (hi > lo && hi > s && lo <= e) {
                    int a = lo > s ? lo : s, b = hi < e ? hi : e;
                    int x0 = tx0 + vcol(d, l, a) * CW - d->hx, x1 = tx0 + vcol(d, l, b) * CW - d->hx;
                    if (hi > e) x1 += CW / 2;
                    gfx_fill(&cc, x0, y - 1, x1 - x0, LH, RGBA(0xB3, 0x88, 0xFF, 100));
                }
                /* indent guides, then the text */
                {
                    int ws = 0;
                    while (s + ws < e && d->buf[s + ws] == ' ') ws++;
                    for (int g = TAB; g < ws; g += TAB) gfx_fill(&cc, tx0 + g * CW - d->hx, y - 1, 1, LH, RGBA(255, 255, 255, 14));
                }
                unsigned char col[4096];
                int n = e - s > 4096 ? 4096 : e - s;
                hl_line(d->lang, d->buf + s, n, d->lst[l], col);
                int vc = 0;
                for (int j = s; j < e; j++) {
                    if (d->buf[j] == '\t') { vc = (vc / TAB + 1) * TAB; continue; }
                    int px = tx0 + vc * CW - d->hx;
                    if (px > ex + ew) break;
                    if (px > tx0 - CW && d->buf[j] != ' ') gfx_char(&cc, &font_mono, px, y + 1, (unsigned char)d->buf[j], HCOL[j - s < n ? col[j - s] : H_DEF]);
                    vc++;
                }
            }
            if (!modal && ui.focused && ((now - last_act) / 500) % 2 == 0 && curl >= d->top && curl < d->top + vis + 1 && (!ui.focus || dragging)) {
                int cx = tx0 + vcol(d, curl, d->cur) * CW - d->hx, cy = ey + 4 + (curl - d->top) * LH;
                gfx_fill(&cc, cx, cy, 2, LH - 2, UI_ACCENT);
            }
            ui_vscroll(&ui, ex + ew - 12, ey + 2, eh - 4, d->nlines, vis, &d->top);
        }

        /* find / replace bar */
        if (find_open && d) {
            gfx_shadow(c, fbx, fby, fbw, fbh, 10, 10, 120);
            gfx_round_fill(c, fbx, fby, fbw, fbh, 10, UI_PANEL);
            gfx_round_rect(c, fbx, fby, fbw, fbh, 10, RGBA(255, 255, 255, 40));
            if (ui_textbox(&ui, fq_x, fq_y, fq_w, fq_h, fq, sizeof fq, "Find")) find_next(false);
            find_stats();
            char cnt[32];
            if (fq[0]) snprintf(cnt, sizeof cnt, fc_total ? "%d of %d" : "no results", fc_index, fc_total);
            else cnt[0] = 0;
            if (fq[0] && fc_total && !fc_index) snprintf(cnt, sizeof cnt, "%d found", fc_total);
            gfx_text(c, &font_ui, fq_x + fq_w + 8, fq_y + 6, cnt, fq[0] && !fc_total ? UI_BAD : UI_TEXT_DIM);
            int bx = fbx + fbw - 8;
            bx -= 24; if (ui_tool(&ui, bx, fq_y, 24, fq_h, "x", false)) { find_open = false; ui.focus = 0; }
            bx -= 28; if (ui_tool(&ui, bx, fq_y, 26, fq_h, ">", false)) find_next(false);
            bx -= 28; if (ui_tool(&ui, bx, fq_y, 26, fq_h, "<", false)) find_next(true);
            bx -= 30; if (ui_tool(&ui, bx, fq_y, 28, fq_h, "Aa", find_case)) find_case = !find_case;
            if (replace_open) {
                if (ui_textbox(&ui, fq_x, rq_y, fq_w, fq_h, rq, sizeof rq, "Replace")) replace_one();
                if (ui_button(&ui, fq_x + fq_w + 8, rq_y, 74, fq_h, "Replace", UI_NORMAL_BTN)) replace_one();
                if (ui_button(&ui, fq_x + fq_w + 88, rq_y, 56, fq_h, "All", UI_NORMAL_BTN)) replace_all();
            }
        }

        /* output pane */
        if (out_open) {
            int px = ex, pw2 = ew;
            gfx_fill(c, px, ey + eh, pw2, 5, C_GUTTER);                    /* splitter */
            gfx_fill(c, px, ey + eh + 2, pw2, 1, UI_BORDER);
            if (ui_hover(&ui, px, ey + eh - 2, pw2, 9) && ui.pressed) out_drag = 1;
            if (out_drag && (ui.buttons & 1)) {
                out_h = H - SB_H - ui.my - 3;
                if (out_h < 80) out_h = 80;
                if (out_h > H - 220) out_h = H - 220;
            } else out_drag = 0;
            gfx_fill(c, px, oy, pw2, oh, RGB(0x10, 0x0F, 0x19));
            gfx_fill(c, px, oy, pw2, 30, UI_PANEL);
            gfx_fill(c, px, oy + 29, pw2, 1, UI_BORDER);
            gfx_text(c, &font_ui_bold, px + 14, oy + 6, "OUTPUT", UI_TEXT_DIM);
            if (status_txt[0]) {
                int sw = gfx_text_width(&font_ui_bold, "OUTPUT");
                gfx_circle_fill(c, px + 14 + sw + 18, oy + 15, 4, status_col);
                gfx_text(c, &font_ui, px + 14 + sw + 30, oy + 6, status_txt, status_col);
            }
            if (ui_tool(&ui, px + pw2 - 64, oy + 3, 56, 24, "Clear", false)) { outlen = 0; nmarks = 0; status_txt[0] = 0; out_top = 0; }
            if (ui_tool(&ui, px + pw2 - 92, oy + 3, 24, 24, "x", false)) out_open = false;
            int in_h = proc_kind == 2 && proc_in >= 0 ? 36 : 0;
            int ty = oy + 34, th = oh - 34 - in_h, ORH = font_mono.height + 3, ovis = th / ORH;
            out_index();
            if (ui_hover(&ui, px, ty, pw2, th) && ui.scroll) { out_top += ui.scroll * 3; out_stick = false; }
            if (out_stick) out_top = nolines - ovis;
            if (out_top > nolines - ovis) out_top = nolines - ovis;
            if (out_top < 0) out_top = 0;
            if (out_top >= nolines - ovis) out_stick = true;
            struct canvas oc = *c;
            canvas_clip(&oc, px, ty, pw2 - 14, th);
            for (int i = out_top; i < nolines && i < out_top + ovis + 1; i++) {
                int s = olines[i], e = i + 1 < nolines ? olines[i + 1] - 1 : outlen, yy = ty + 2 + (i - out_top) * ORH;
                if (e > s && out[e - 1] == '\n') e--;
                char lp[PATH_MAX];
                int ln;
                bool loc = parse_loc(out + s, e - s, lp, &ln);
                bool hov = loc && ui_hover(&ui, px, yy, pw2 - 14, ORH);
                color_t col = UI_TEXT;
                if (has(out + s, e - s, "error") || has(out + s, e - s, "Error")) col = UI_BAD;
                else if (has(out + s, e - s, "warning")) col = UI_WARN;
                else if (loc) col = UI_BAD;
                else if (out[s] == '$') col = UI_ACCENT;
                else if (out[s] == '[') col = UI_TEXT_DIM;
                if (hov) gfx_fill(&oc, px, yy - 1, pw2, ORH, RGBA(255, 255, 255, 14));
                gfx_text_n(&oc, &font_mono, px + 14, yy, out + s, e - s > 300 ? 300 : e - s, col);
                if (hov) {
                    gfx_fill(&oc, px + 14, yy + font_mono.height, gfx_text_width_n(&font_mono, out + s, e - s > 300 ? 300 : e - s), 1, col);
                    if (ui.pressed) { jump_to(lp, ln, vis); ui.pressed = false; }
                }
            }
            if (!outlen) gfx_text(c, &font_ui, px + 14, ty + 8, "Build and run output appears here. Click an error to jump to it.", UI_TEXT_DIM);
            ui_vscroll(&ui, px + pw2 - 12, ty, th, nolines, ovis, &out_top);
            if (in_h) {
                gfx_fill(c, px, oy + oh - in_h, pw2, 1, UI_BORDER);
                gfx_text(c, &font_ui, px + 14, oy + oh - in_h + 10, "stdin >", UI_TEXT_DIM);
                if (ui_textbox(&ui, px + 80, oy + oh - in_h + 3, pw2 - 94, 30, stdin_line, sizeof stdin_line, "type input for the program, Enter sends")) {
                    int n = strlen(stdin_line);
                    stdin_line[n++] = '\n';
                    write(proc_in, stdin_line, n);
                    out_append(stdin_line, n);
                    stdin_line[0] = 0;
                }
            }
        }

        /* status bar */
        gfx_fill(c, 0, H - SB_H, W, SB_H, UI_PANEL);
        gfx_fill(c, 0, H - SB_H, W, 1, UI_BORDER);
        {
            int y = H - SB_H + (SB_H - font_ui.height) / 2;
            char st[96];
            if (d) {
                int lo, hi, curl = line_of(d, d->cur);
                sel_range(&lo, &hi);
                snprintf(st, sizeof st, "Ln %d, Col %d", curl + 1, vcol(d, curl, d->cur) + 1);
                gfx_text(c, &font_ui, 14, y, st, UI_TEXT_DIM);
                if (hi > lo) { snprintf(st, sizeof st, "%d selected", hi - lo); gfx_text(c, &font_ui, 140, y, st, UI_ACCENT); }
                static const char *const lname[] = { "Plain Text", "C", "Lua" };
                snprintf(st, sizeof st, "Spaces: 4    %s", lname[d->lang]);
                gfx_text(c, &font_ui, W - 14 - gfx_text_width(&font_ui, st), y, st, UI_TEXT_DIM);
            }
            if (msg[0] && now < msg_until) gfx_text_center(c, &font_ui, 0, y, W, msg, msg_bad ? UI_BAD : UI_GOOD);
        }

        /* ---- dialogs ---- */
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.dbl = sd; ui.rpressed = srp; ui.scroll = ssc;
            ui.nkeys = nk;
            memcpy(ui.keys, saved_keys, sizeof saved_keys);
            if (mode >= M_OPEN && mode <= M_GOTO) {
                static const char *const titles[] = { "", "Open file", "Save as", "New file", "New folder", "Open folder", "Go to line" };
                static const char *const oks[] = { "", "Open", "Save", "Create", "Create", "Open", "Go" };
                int r = ui_prompt(&ui, titles[mode], oks[mode], prompt, sizeof prompt);
                if (r == 1 && prompt[0]) {
                    bool done = true;
                    if (mode == M_OPEN) done = open_file(prompt) != NULL;
                    else if (mode == M_SAVEAS) {
                        if (d && doc_save(d, prompt)) { say(false, "Saved %s", base(d->path)); tree_refresh(); } else done = false;
                    } else if (mode == M_NEWFILE) {
                        int fd = open(prompt, O_WRONLY | O_CREAT | O_TRUNC);
                        if (fd < 0) { say(true, "Cannot create %s", prompt); done = false; }
                        else { close(fd); tree_refresh(); done = open_file(prompt) != NULL; }
                    } else if (mode == M_NEWDIR) {
                        if (mkdir(prompt, 0755) < 0) { say(true, "Cannot create %s", prompt); done = false; }
                        else tree_refresh();
                    } else if (mode == M_OPENDIR) set_root(prompt);
                    else if (mode == M_GOTO && d) goto_line(d, atoi(prompt), vis);
                    if (done) mode = M_NONE;
                } else if (r == -1) mode = M_NONE;
            } else if (mode == M_CLOSETAB) {
                int r = dlg3(&ui, "Unsaved changes", docs[pending_close]->path[0] ? base(docs[pending_close]->path) : "untitled", "Save", "Don't save");
                if (r == 1) {
                    struct doc *x = docs[pending_close];
                    if (x->path[0] && doc_save(x, x->path)) { close_doc(pending_close); mode = M_NONE; }
                    else { select_doc(pending_close); mode = M_SAVEAS; snprintf(prompt, sizeof prompt, "%s/untitled.c", root); }
                } else if (r == 2) { close_doc(pending_close); mode = M_NONE; }
                else if (r == -1) mode = M_NONE;
            } else if (mode == M_QUIT) {
                int r = dlg3(&ui, "Unsaved changes", "Some files have unsaved edits.", "Save all", "Discard");
                if (r == 1) { save_all(); if (!any_modified()) break; mode = M_NONE; }
                else if (r == 2) break;
                else if (r == -1) mode = M_NONE;
            }
        }

        /* window title */
        {
            char t[128];
            snprintf(t, sizeof t, "%s%s - Code", d && d->modified ? "* " : "", d ? (d->path[0] ? base(d->path) : "untitled") : base(root));
            if (strcmp(t, last_title)) { strcpy(last_title, t); gui_set_title(w, t); }
        }
        ui_end(&ui);
    }
    if (proc_pid > 0) kill(proc_pid, SIGKILL);
    gui_window_destroy(w);
    return 0;
}
