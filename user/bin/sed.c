/* sed [-nEr] [-i[SUF]] [-e SCRIPT]... [-f FILE] [SCRIPT] [file...]
 * addresses: N  $  /re/  first~step  addr,addr  addr,+N  !   commands: s y d D p P q Q n N = a i c r h H g G x b t T : { }
 * s flags: g p N I.  Regexes are libc BRE (ERE with -E); `&` and \1..\9 in replacements. */
#include <vutil.h>
#include <regex.h>
#include <ctype.h>

typedef struct { int t; long n, step; regex_t *re; } Addr;     /* t: 0 none, 'n' line, '$', '/', '~' step, '+' count (addr2) */
typedef struct {
    Addr a1, a2;
    int neg, active, c, target, g, p, nth;
    long endl;
    char *text, *repl;
    regex_t *re;
    unsigned char *map;
} Cmd;
typedef struct { char *p; size_t n, cap; } Buf;

static Cmd *cmds;
static int ncmd, nflag, ere, inplace;
static const char *suffix;
static const char *sp;                 /* script cursor */
static regex_t *lre;                   /* last regex used (empty regex = reuse) */

static void die(const char *m) { fprintf(stderr, "sed: %s\n", m); exit(1); }

/* ---- script parsing ---- */
static void skipws(void) { while (*sp == ' ' || *sp == '\t') sp++; }

/* text up to the unescaped delimiter d; `\d` -> d, and with rx `\n` -> newline. Leaves other escapes alone. */
static char *delim(int d, int rx) {
    size_t cap = 32, n = 0;
    char *b = malloc(cap);
    while (*sp && *sp != d) {
        if (n + 3 >= cap) b = realloc(b, cap *= 2);
        if (*sp == '\\' && sp[1]) {
            if (sp[1] == d && d != '\n') { b[n++] = d; sp += 2; continue; }
            if (rx && sp[1] == 'n') { b[n++] = '\n'; sp += 2; continue; }
            if (rx && sp[1] == 't') { b[n++] = '\t'; sp += 2; continue; }
            b[n++] = *sp++;
        } else if (*sp == '\n' && rx) die("unterminated regex");
        b[n++] = *sp++;
    }
    if (!*sp) die("unterminated `s' command or address");
    sp++;
    b[n] = 0;
    return b;
}

static regex_t *comp(const char *s, int icase) {
    if (!*s) return NULL;
    regex_t *r = malloc(sizeof *r);
    int e = regcomp(r, s, (ere ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
    if (e) { char m[100]; regerror(e, r, m, sizeof m); fprintf(stderr, "sed: bad regex '%s': %s\n", s, m); exit(1); }
    return r;
}

static int addr(Addr *a, int second) {
    skipws();
    if (*sp >= '0' && *sp <= '9') {
        a->n = strtol(sp, (char **)&sp, 10);
        if (!second && *sp == '~') { sp++; a->step = strtol(sp, (char **)&sp, 10); a->t = '~'; }
        else a->t = 'n';
    } else if (*sp == '$') { sp++; a->t = '$'; }
    else if (second && *sp == '+') { sp++; a->n = strtol(sp, (char **)&sp, 10); a->t = '+'; }
    else if (*sp == '/' || (*sp == '\\' && sp[1])) {
        int d = '/';
        if (*sp == '\\') sp++, d = *sp;
        sp++;
        char *s = delim(d, 1);
        int ic = 0;
        for (; *sp == 'I'; sp++) ic = 1;
        a->re = comp(s, ic);
        free(s);
        a->t = '/';
    } else return 0;
    return 1;
}

static char *label(void) {
    skipws();
    const char *s = sp;
    while (*sp && *sp != ';' && *sp != '\n' && *sp != ' ' && *sp != '\t' && *sp != '}') sp++;
    return strndup(s, sp - s);
}

/* a/i/c text: `a\<newline>text`, `a\text` or `a text`; backslash-newline continues */
static char *text(void) {
    skipws();
    if (*sp == '\\') {
        const char *q = ++sp;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '\n') sp = q + 1;
    }
    size_t cap = 32, n = 0;
    char *b = malloc(cap);
    while (*sp && *sp != '\n') {
        if (n + 2 >= cap) b = realloc(b, cap *= 2);
        if (*sp == '\\' && sp[1]) sp++;
        b[n++] = *sp++;
    }
    b[n] = 0;
    return b;
}

static void end_cmd(void) {
    skipws();
    if (*sp == ';' || *sp == '\n') sp++;
    else if (*sp && *sp != '}') die("extra characters after command");
}

static char *parse_repl(char *r) {
    char *o = malloc(strlen(r) * 2 + 1), *w = o;
    for (; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            if (*r >= '0' && *r <= '9') { *w++ = '\377'; *w++ = *r; }
            else if (*r == 'n') *w++ = '\n';
            else if (*r == 't') *w++ = '\t';
            else if (strchr("ULulE", *r)) { *w++ = '\376'; *w++ = *r; }      /* case conversion */
            else *w++ = *r;                 /* \& \\ \/ and backslash-newline */
        } else if (*r == '&') { *w++ = '\377'; *w++ = '0'; }
        else *w++ = *r;
    }
    *w = 0;
    return o;                               /* ponytail: a literal 0xFF byte in a replacement is taken as a group marker */
}

static void parse(const char *script) {
    int stack[64], sd = 0;
    sp = script;
    for (;;) {
        while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == ';') sp++;
        if (!*sp) break;
        cmds = realloc(cmds, (ncmd + 1) * sizeof *cmds);
        Cmd *c = &cmds[ncmd];
        memset(c, 0, sizeof *c);
        if (addr(&c->a1, 0)) {
            skipws();
            if (*sp == ',') { sp++; if (!addr(&c->a2, 1)) die("unexpected `,'"); }
        }
        skipws();
        while (*sp == '!') { c->neg = 1; sp++; skipws(); }
        c->c = *sp++;
        switch (c->c) {
        case '{': if (sd >= 64) die("blocks nested too deeply"); stack[sd++] = ncmd; break;
        case '}':
            if (!sd) die("unexpected `}'");
            cmds[stack[--sd]].target = ncmd;
            end_cmd();
            break;
        case 's': {
            int d = *sp++;
            if (!d || d == '\n' || d == '\\') die("bad delimiter for `s'");
            char *re = delim(d, 1), *rp = delim(d, 0);
            int ic = 0;
            c->nth = 0;
            for (;; sp++) {
                if (*sp == 'g') c->g = 1;
                else if (*sp == 'p') c->p = 1;
                else if (*sp == 'I' || *sp == 'i') ic = 1;
                else if (*sp >= '1' && *sp <= '9') { c->nth = strtol(sp, (char **)&sp, 10); sp--; }
                else break;
            }
            c->re = comp(re, ic);
            c->repl = parse_repl(rp);
            free(re); free(rp);
            end_cmd();
            break;
        }
        case 'y': {
            int d = *sp++;
            char *a = delim(d, 0), *b = delim(d, 0);
            unsigned char *m = malloc(256);
            for (int i = 0; i < 256; i++) m[i] = i;
            char *p = a, *q = b;
            for (; *p && *q; p++, q++) {
                int x = *p, y = *q;
                if (x == '\\' && p[1]) { p++; x = *p == 'n' ? '\n' : *p; }
                if (y == '\\' && q[1]) { q++; y = *q == 'n' ? '\n' : *q; }
                m[x & 255] = y;
            }
            if (*p || *q) die("strings for `y' command are different lengths");
            c->map = m;
            free(a); free(b);
            end_cmd();
            break;
        }
        case 'a': case 'i': case 'c': c->text = text(); break;
        case 'r': skipws(); { const char *s = sp; while (*sp && *sp != '\n') sp++; c->text = strndup(s, sp - s); } break;
        case 'b': case 't': case 'T': c->text = label(); end_cmd(); break;
        case ':': c->text = label(); end_cmd(); break;
        case 'd': case 'D': case 'p': case 'P': case 'n': case 'N': case '=': case 'h': case 'H': case 'g': case 'G': case 'x':
            end_cmd();
            break;
        case 'q': case 'Q':
            skipws();
            if (*sp >= '0' && *sp <= '9') c->nth = strtol(sp, (char **)&sp, 10);
            end_cmd();
            break;
        default: fprintf(stderr, "sed: unknown command: `%c'\n", c->c); exit(1);
        }
        ncmd++;
    }
    if (sd) die("unmatched `{'");
    for (int i = 0; i < ncmd; i++) {
        Cmd *c = &cmds[i];
        if (c->c != 'b' && c->c != 't' && c->c != 'T') continue;
        c->target = ncmd;
        if (*c->text) {
            int j;
            for (j = 0; j < ncmd; j++) if (cmds[j].c == ':' && !strcmp(cmds[j].text, c->text)) break;
            if (j == ncmd) { fprintf(stderr, "sed: can't find label for jump to `%s'\n", c->text); exit(1); }
            c->target = j;
        }
    }
}

/* ---- buffers, input, output ---- */
static void bcat(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) b->p = realloc(b->p, b->cap = (b->n + n + 1) * 2);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bset(Buf *b, const char *s, size_t n) { b->n = 0; bcat(b, s, n); }

static char **files;
static int nfiles, fidx, rc;
static char *ibuf;
static size_t ilen, ipos;
static long lineno;
static FILE *out;
static int pend_nl, cur_noeol;

static int load_next(void) {
    while (fidx < nfiles) {
        free(ibuf);
        ibuf = slurp("sed", files[fidx++], &ilen);
        ipos = 0;
        if (!ibuf) { rc = 2; ilen = 0; continue; }
        if (ilen) return 1;
    }
    return 0;
}
static int more(void) { return ipos < ilen || load_next(); }

static int readline(Buf *b, int append) {
    if (!more()) return 0;
    char *e = memchr(ibuf + ipos, '\n', ilen - ipos);
    size_t n = e ? (size_t)(e - (ibuf + ipos)) : ilen - ipos;
    if (append) bcat(b, "\n", 1); else b->n = 0;
    bcat(b, ibuf + ipos, n);
    ipos += n + (e != NULL);
    cur_noeol = !e;
    lineno++;
    return 1;
}

static void wr(const char *s, size_t n) {
    if (pend_nl) { putc('\n', out); pend_nl = 0; }
    fwrite(s, 1, n, out);
}
static void wline(const char *s, size_t n, int noeol) {
    wr(s, n);
    if (noeol) pend_nl = 1; else putc('\n', out);
}

typedef struct { int file; char *s; } Ap;
static Ap *aps;
static int nap;
static void flush_ap(void) {
    for (int i = 0; i < nap; i++) {
        if (aps[i].file) {
            size_t n;
            char *b = slurp("sed", aps[i].s, &n);      /* a missing r file is silently skipped like GNU */
            if (b) { wr(b, n); free(b); }
        } else wline(aps[i].s, strlen(aps[i].s), 0);
    }
    nap = 0;
}

/* ---- execution ---- */
static int rmatch(regex_t *re, const Buf *b, int *so, int *eo) {
    if (!re) re = lre;
    if (!re) die("no previous regular expression");
    lre = re;
    regmatch_t m[1] = { { 0, (regoff_t)b->n } };
    if (regexec(re, b->p, 1, m, REG_STARTEND)) return 0;
    if (so) { *so = m[0].rm_so; *eo = m[0].rm_eo; }
    return 1;
}

static int amatch(const Addr *a, const Buf *ps) {
    switch (a->t) {
    case 'n': return lineno == a->n;
    case '$': return !more();
    case '/': return rmatch(a->re, ps, NULL, NULL);
    case '~': return a->step > 0 ? lineno >= a->n && (lineno - a->n) % a->step == 0 : lineno == a->n;
    }
    return 1;
}

static int selected(Cmd *c, const Buf *ps) {
    int r;
    if (!c->a1.t) r = 1;
    else if (!c->a2.t) r = amatch(&c->a1, ps);
    else if (c->active) {
        r = 1;
        switch (c->a2.t) {
        case 'n': if (lineno >= c->a2.n) c->active = 0; break;
        case '$': if (!more()) c->active = 0; break;
        case '/': if (rmatch(c->a2.re, ps, NULL, NULL)) c->active = 0; break;
        case '+': if (lineno >= c->endl) c->active = 0; break;
        }
    } else if (amatch(&c->a1, ps)) {
        r = 1;
        switch (c->a2.t) {
        case 'n': c->active = c->a2.n > lineno; break;
        case '$': c->active = more(); break;
        case '+': c->endl = lineno + c->a2.n; c->active = c->a2.n > 0; break;
        default: c->active = 1;
        }
    } else r = 0;
    return r != c->neg;
}

static int subst(Cmd *c, Buf *ps, Buf *tmp) {
    int count = 0, done = 0, lasteo = -1;
    size_t pos = 0;
    int want = c->nth ? c->nth : 1;
    regex_t *re = c->re ? c->re : lre;
    if (!re) die("no previous regular expression");
    lre = re;
    tmp->n = 0; tmp->p = tmp->p ? tmp->p : (bcat(tmp, "", 0), tmp->p);
    tmp->p[0] = 0;
    while (pos <= ps->n) {
        regmatch_t m[10];
        m[0].rm_so = pos; m[0].rm_eo = ps->n;
        if (regexec(re, ps->p, 10, m, REG_STARTEND)) break;
        int so = m[0].rm_so, eo = m[0].rm_eo;
        if (so == eo && so == lasteo) {            /* empty match touching the previous one doesn't count */
            if ((size_t)so < ps->n) bcat(tmp, ps->p + pos, so - pos + 1);
            pos = so + 1;
            continue;
        }
        count++;
        lasteo = eo;
        bcat(tmp, ps->p + pos, so - pos);
        if (count >= want) {
            int mode = 0, once = 0;
            for (const char *r = c->repl; *r; r++) {
                const char *s = r;
                size_t n = 1;
                if (*r == '\376') { int k = *++r; if (k == 'E') mode = once = 0; else if (k == 'U' || k == 'L') mode = k; else once = k; continue; }
                if (*r == '\377') {
                    int g = *++r - '0';
                    if (g > (int)re->re_nsub || m[g].rm_so < 0) continue;
                    s = ps->p + m[g].rm_so; n = m[g].rm_eo - m[g].rm_so;
                }
                for (size_t k = 0; k < n; k++) {
                    int ch = (unsigned char)s[k], cs = once ? once : mode;
                    if (cs == 'U' || cs == 'u') ch = toupper(ch); else if (cs == 'L' || cs == 'l') ch = tolower(ch);
                    once = 0;
                    char cc = ch;
                    bcat(tmp, &cc, 1);
                }
            }
            done = 1;
        } else bcat(tmp, ps->p + so, eo - so);
        if (so == eo) { if ((size_t)so < ps->n) bcat(tmp, ps->p + so, 1); pos = so + 1; }
        else pos = eo;
        if (done && !c->g) break;
    }
    if (!done) return 0;
    if (pos < ps->n) bcat(tmp, ps->p + pos, ps->n - pos);
    Buf t = *ps; *ps = *tmp; *tmp = t;
    return 1;
}

static void run(void) {
    static Buf ps, hs, tmp;
    int restart = 0;
    lineno = 0;
    while (restart || readline(&ps, 0)) {
        int pc = 0, tflag = 0;
        restart = 0;
        while (pc < ncmd) {
            Cmd *c = &cmds[pc];
            if (c->c == '}' || c->c == ':') { pc++; continue; }
            if (!selected(c, &ps)) { pc = c->c == '{' ? c->target + 1 : pc + 1; continue; }
            pc++;
            switch (c->c) {
            case 's': if (subst(c, &ps, &tmp)) { tflag = 1; if (c->p) wline(ps.p, ps.n, cur_noeol); } break;
            case 'y': for (size_t i = 0; i < ps.n; i++) ps.p[i] = c->map[(unsigned char)ps.p[i]]; break;
            case 'd': goto endcycle;
            case 'D': {
                char *nl = memchr(ps.p, '\n', ps.n);
                if (!nl) goto endcycle;
                ps.n -= nl + 1 - ps.p;
                memmove(ps.p, nl + 1, ps.n + 1);
                restart = 1;
                goto endcycle;
            }
            case 'p': wline(ps.p, ps.n, cur_noeol); break;
            case 'P': { char *nl = memchr(ps.p, '\n', ps.n); wline(ps.p, nl ? (size_t)(nl - ps.p) : ps.n, nl ? 0 : cur_noeol); break; }
            case 'q': case 'Q':
                rc = c->nth;
                if (c->c == 'q') { if (!nflag) wline(ps.p, ps.n, cur_noeol); flush_ap(); }
                return;
            case 'n': case 'N':
                if (!more()) { if (!nflag) wline(ps.p, ps.n, cur_noeol); flush_ap(); return; }   /* GNU: print, then exit */
                if (c->c == 'n' && !nflag) wline(ps.p, ps.n, cur_noeol);
                flush_ap();
                readline(&ps, c->c == 'N');
                break;
            case '=': { char b[24]; int n = snprintf(b, sizeof b, "%ld", lineno); wline(b, n, 0); break; }
            case 'a': case 'r': aps = realloc(aps, (nap + 1) * sizeof *aps); aps[nap].file = c->c == 'r'; aps[nap++].s = c->text; break;
            case 'i': wline(c->text, strlen(c->text), 0); break;
            case 'c':
                if (!c->a2.t || !c->active || c->neg) wline(c->text, strlen(c->text), 0);   /* a range prints once, at its end */
                goto endcycle;
            case 'h': bset(&hs, ps.p, ps.n); break;
            case 'H': bcat(&hs, "\n", 1); bcat(&hs, ps.p, ps.n); break;
            case 'g': bset(&ps, hs.p ? hs.p : "", hs.n); break;
            case 'G': bcat(&ps, "\n", 1); if (hs.p) bcat(&ps, hs.p, hs.n); break;
            case 'x': { Buf t = ps; ps = hs; hs = t; if (!ps.p) bset(&ps, "", 0); break; }
            case 'b': pc = c->target; break;
            case 't': if (tflag) { tflag = 0; pc = c->target; } break;
            case 'T': if (!tflag) pc = c->target; else tflag = 0; break;
            }
        }
        if (!nflag) wline(ps.p, ps.n, cur_noeol);
    endcycle:
        flush_ap();
    }
}

static int one_file(void) {
    for (int i = 0; i < ncmd; i++) cmds[i].active = 0;
    run();
    return rc;
}

int main(int argc, char **argv) {
    Buf sc = { 0 };
    int i = 1, have = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            switch (*f) {
            case 'n': nflag = 1; break;
            case 'E': case 'r': ere = 1; break;
            case 's': break;
            case 'i': inplace = 1; if (f[1]) suffix = f + 1; f = " "; break;
            case 'e': case 'f': {
                const char *a = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!a) goto usage;
                if (*f == 'e') bcat(&sc, a, strlen(a));
                else { size_t n; char *b = slurp("sed", a, &n); if (!b) return 1; if (n && b[n - 1] == '\n') n--; bcat(&sc, b, n); free(b); }
                bcat(&sc, "\n", 1);
                have = 1; f = " ";
                break;
            }
            default: goto usage;
            }
    }
    if (!have) {
        if (i >= argc) goto usage;
        bcat(&sc, argv[i], strlen(argv[i]));
        i++;
    }
    parse(sc.p);
    out = stdout;
    if (!inplace) {
        static char *dash[] = { "-" };
        files = i < argc ? argv + i : dash;
        nfiles = i < argc ? argc - i : 1;
        one_file();
    } else {
        for (; i < argc; i++) {
            char tmpn[PATH_MAX], bak[PATH_MAX];
            snprintf(tmpn, sizeof tmpn, "%s.sedtmp", argv[i]);
            out = fopen(tmpn, "w");
            if (!out) { WARN("sed", tmpn); rc = 4; continue; }
            files = argv + i; nfiles = 1; fidx = 0; pend_nl = 0;
            one_file();
            fclose(out);
            if (suffix) {
                if (strchr(suffix, '*')) {            /* GNU: * stands for the base name */
                    const char *b = base_name(argv[i]);
                    snprintf(bak, sizeof bak, "%.*s", (int)(b - argv[i]), argv[i]);
                    for (const char *s = suffix; *s; s++) {
                        if (*s == '*') strncat(bak, b, sizeof bak - strlen(bak) - 1);
                        else strncat(bak, s, 1);
                    }
                } else snprintf(bak, sizeof bak, "%s%s", argv[i], suffix);
                rename(argv[i], bak);
            }
            rename(tmpn, argv[i]);
        }
    }
    fflush(stdout);
    return rc;
usage:
    fputs("usage: sed [-nEr] [-i[SUF]] [-e SCRIPT] [-f FILE] [SCRIPT] [file...]\n", stderr);
    return 1;
}
