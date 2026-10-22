/* regex_diff.c - differential test of user/lib/regex.c (built 64-bit as vibe_*) against glibc's regcomp/regexec.
 * Hand-written corpus + random fuzz. Overall match is always compared; submatches are compared too
 * (counted separately: glibc and a Pike VM can legitimately disagree on exotic ambiguous captures). */
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>

int vibe_regcomp(void *re, const char *pat, int flags);
int vibe_regexec(const void *re, const char *s, size_t n, regmatch_t *pm, int flags);
void vibe_regfree(void *re);

static sigjmp_buf jb;
static long skipped;
static void onalrm(int x) { siglongjmp(jb, 1); }
/* glibc's matcher can take exponential time on anchors inside loops; give it 150 ms */
static int gexec(const regex_t *g, const char *s, regmatch_t *gm, int ef, int *timeout) {
    struct itimerval it = { { 0, 0 }, { 0, 150000 } }, off = { { 0, 0 }, { 0, 0 } };
    int r;
    *timeout = 0;
    signal(SIGALRM, onalrm);
    if (sigsetjmp(jb, 1)) { *timeout = 1; return -1; }
    setitimer(ITIMER_REAL, &it, NULL);
    r = regexec(g, s, 10, gm, ef);
    setitimer(ITIMER_REAL, &off, NULL);
    return r;
}
static long checks, fails, subdiff, compdiff;
static int verbose;

static void one(const char *pat, const char *s, int cf, int ef, int strict) {
    regex_t g;
    union { size_t nsub; char b[64]; } v;
    int gr = regcomp(&g, pat, cf), vr = vibe_regcomp(&v, pat, cf);
    checks++;
    if ((gr != 0) != (vr != 0)) {
        if (compdiff++ < 10 || verbose) printf("COMPILE pat=[%s] cf=%d glibc=%d vibe=%d\n", pat, cf, gr, vr);
        if (gr == 0) regfree(&g);
        if (vr == 0) vibe_regfree(&v);
        fails++;
        return;
    }
    if (gr) return;
    if (getenv("RXTRACE")) fprintf(stderr, "[%s] [%s] %d %d\n", pat, s, cf, ef);
    regmatch_t gm[10], vm[10];
    for (int i = 0; i < 10; i++) gm[i].rm_so = gm[i].rm_eo = vm[i].rm_so = vm[i].rm_eo = -7;
    int to, ge = gexec(&g, s, gm, ef, &to), ve = vibe_regexec(&v, s, 10, vm, ef);
    if (to) { skipped++; vibe_regfree(&v); return; }
    checks++;
    if (ge != ve) { fails++; printf("MATCH pat=[%s] s=[%s] cf=%d ef=%d glibc=%d vibe=%d\n", pat, s, cf, ef, ge, ve); }
    else if (!ge) {
        if (gm[0].rm_so != vm[0].rm_so || gm[0].rm_eo != vm[0].rm_eo) {
            fails++; printf("EXTENT pat=[%s] s=[%s] cf=%d ef=%d glibc=(%d,%d) vibe=(%d,%d)\n", pat, s, cf, ef, gm[0].rm_so, gm[0].rm_eo, vm[0].rm_so, vm[0].rm_eo);
        } else {
            int n = g.re_nsub < 9 ? g.re_nsub : 9, bad = 0;
            for (int i = 1; i <= n; i++) if (gm[i].rm_so != vm[i].rm_so || gm[i].rm_eo != vm[i].rm_eo) bad = 1;
            if (bad) {
                subdiff++;
                if (strict || verbose) { fails += strict; printf("%s pat=[%s] s=[%s] cf=%d:", strict ? "SUBMATCH" : "submatch~", pat, s, cf);
                    for (int i = 1; i <= n; i++) printf(" g(%d,%d)/v(%d,%d)", gm[i].rm_so, gm[i].rm_eo, vm[i].rm_so, vm[i].rm_eo);
                    printf("\n"); }
            }
        }
        /* NOSUB / nmatch=0 path must agree on yes/no */
    }
    regmatch_t dummy[1];
    int v0 = vibe_regexec(&v, s, 0, dummy, ef);
    checks++;
    if ((v0 == 0) != (ve == 0)) { fails++; printf("BOOLMODE pat=[%s] s=[%s]\n", pat, s); }
    regfree(&g); vibe_regfree(&v);
}

static const char *strs[] = { "", "a", "b", "ab", "abc", "aab", "abab", "ababab", "aaaa", "xabcx", "abcabc", "hello world", "Hello World",
    "foo.bar", "a\nb", "ab\ncd\n", "\nab", "abba", "aXb", "a-b", "[x]", "foo123bar", "  lead", "tail  ", "a b  c", "the cat sat",
    "12:34:56", "x=1, y=22", "AbC", "aaa bbb", "abcd", "abbbc", "a*b", "a+b", "a|b", "(a)", "a{2}", "a^b", "a$b", "}{", "ba", "cab", NULL };

static const char *bre[] = { "a", "ab", "a*", "a*b", "^a", "a$", "^a*$", "^$", "a.b", "[abc]", "[^abc]", "[a-c]*", "[[:alpha:]]*", "[[:digit:]]\\{2\\}",
    "\\(a\\)", "\\(ab\\)*", "\\(a*\\)\\(b*\\)", "\\(a\\|b\\)*", "a\\|b", "ab\\|abc", "a\\+", "a\\?b", "\\(a\\)\\1", "\\(a*\\)\\1", "\\(ab\\)\\1",
    "\\([a-z]*\\) \\1", "a\\{2\\}", "a\\{2,\\}", "a\\{1,2\\}", "a\\{0,1\\}b", "\\(a\\{2\\}\\)*", "*a", "^*a", "\\(*a\\)", "a**",
    "[]a]", "[^]a]", "[a-]", "[[.-.]]", "[[=a=]]", "[[:upper:][:digit:]]", "\\w\\+", "\\W", "\\s", "\\S\\+", "\\bfoo", "o\\b", "\\Bo", "\\<w", "d\\>",
    "foo\\.bar", "a^b", "a$b", "\\(^a\\)", "\\(a$\\)", "^\\(a\\|b\\)", "x\\(a\\|b\\)$", ".*", ".\\+", "\\(.\\)\\(.\\)\\2\\1", "[0-9]\\+:[0-9]\\+", "\\(a\\|ab\\)\\(c\\|bcd\\)",
    "\\(a*\\)*", "\\(a*\\)\\+", "\\(a\\|b\\)\\{2\\}", "a\\{,2\\}", ".", "\\.", "\\*", "[*]", "\\(\\)", "()", "a\\{1", "\\(a", "a\\)", "[a", "[[:foo:]]", "a\\{2,1\\}", "\\1", "a\\", "[z-a]", NULL };

static const char *ere[] = { "a", "ab", "a*", "a+", "a?", "a|b", "ab|abc", "(a)", "(ab)*", "(a|b)*", "(a*)(b*)", "(a*)+", "(a|ab)(c|bcd)", "a{2}", "a{2,}", "a{1,2}",
    "a{0,1}b", "(a{2})*", "^a", "a$", "^$", "^(a|b)", "x(a|b)$", "(^a)", "(a$)", "a|^b", "a$|b", "[[:alpha:]]+", "[[:digit:]]{2,3}", "[^abc]+", "(.)(.)\\2\\1",
    "(a)\\1", "([a-z]+) \\1", "\\w+", "\\W+", "\\s+", "\\bfoo", "o\\b", "\\<w", "d\\>", "a.b", ".*", ".+", "(a|b|c)+", "((a)|(b))+", "(a(b)?)+", "(a*)*", "(a*)+b", "(a|b)*abb",
    "a\\.b", "a\\*", "a\\+", "a\\{2\\}", "\\(a\\)", "[0-9]+:[0-9]+:[0-9]+", "[a-z]+=[0-9]+", "(foo|foobar)(bar)?", "(a|aa)+", "x*", "a{,2}", "()", "(|a)", "(a|)", "a||b",
    "*a", "+a", "a{1", "(a", "a)", "a{2,1}", "[a", "\\", "a{1}{2}", "a**", "a+?", "(ab|a)(bc|c)?", "^*", "a|*b", "(a)|b", "((a))", "(((a)))\\3", NULL };

static const char alpha[] = "ab";
static char *rnd_re(int ere_, int d) {
    char b[256]; int n = 0;
    int terms = 1 + rand() % 3;
    for (int t = 0; t < terms; t++) {
        int k = rand() % 12;
        if (k < 5) b[n++] = alpha[rand() % 2];
        else if (k == 5) b[n++] = '.';
        else if (k == 6) { strcpy(b + n, rand() % 2 ? "[ab]" : "[^a]"); n += 4; }
        else if (k == 7 && d < 1) {
            char *in = rnd_re(ere_, d + 1);
            n += sprintf(b + n, ere_ ? "(%s)" : "\\(%s\\)", in); free(in);
        } else if (k == 8 && d < 1) {
            char *x = rnd_re(ere_, d + 1), *y = rnd_re(ere_, d + 1);
            n += sprintf(b + n, ere_ ? "(%s|%s)" : "\\(%s\\|%s\\)", x, y); free(x); free(y);
        } else b[n++] = alpha[rand() % 2];
        int q = rand() % 7;
        if (n && b[n - 1] != '^') {
            if (q == 0) b[n++] = '*';
            else if (q == 1) n += sprintf(b + n, ere_ ? "+" : "\\+");
            else if (q == 2) n += sprintf(b + n, ere_ ? "?" : "\\?");
            else if (q == 3) n += sprintf(b + n, ere_ ? "{1,2}" : "\\{1,2\\}");
        }
    }
    if (!d && rand() % 8 == 0) { memmove(b + 1, b, n); b[0] = '^'; n++; }
    if (!d && rand() % 8 == 0) b[n++] = '$';
    b[n] = 0;
    return strdup(b);
}
static void rnd_str(char *s) {
    int n = rand() % 9, i;
    for (i = 0; i < n; i++) s[i] = "aabb\n"[rand() % (rand() % 6 ? 4 : 5)];
    s[i] = 0;
}

int main(int argc, char **argv) {
    verbose = argc > 1;
    if (getenv("RXTRACE")) setvbuf(stdout, NULL, _IONBF, 0);
    static const int cfs[] = { 0, REG_ICASE, REG_NEWLINE, REG_ICASE | REG_NEWLINE };
    for (int e = 0; e < 2; e++)
        for (const char **p = e ? ere : bre; *p; p++)
            for (int c = 0; c < 4; c++)
                for (const char **s = strs; *s; s++) {
                    int cf = cfs[c] | (e ? REG_EXTENDED : 0);
                    one(*p, *s, cf, 0, 1);
                    if (c == 0 || c == 2) { one(*p, *s, cf, REG_NOTBOL, 1); one(*p, *s, cf, REG_NOTEOL, 1); }
                }
    srand(12345);
    long before = checks;
    for (int i = 0; i < 40000; i++) {
        int e = rand() & 1;
        char *p = rnd_re(e, 0), s[16];
        rnd_str(s);
        one(p, s, (e ? REG_EXTENDED : 0) | (rand() % 4 == 0 ? REG_NEWLINE : 0) | (rand() % 4 == 0 ? REG_ICASE : 0), 0, 0);
        free(p);
    }
    printf("regex diff: %ld skipped (glibc timeouts)\n", skipped);
    printf("regex diff: %ld checks (%ld fuzz), %ld failures, %ld capture-only disagreements (fuzz+corpus), %ld compile disagreements\n",
           checks, checks - before, fails, subdiff, compdiff);
    return fails != 0;
}
