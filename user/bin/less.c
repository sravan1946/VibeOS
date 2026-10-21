/* less [file]   arrows/j/k line, space/b/PgDn/PgUp page, d/u half page, g/G top/bottom, left/right pan,
 *               /text search (n/N next/prev, smart-case substring), q quit.  Reads stdin when piped. */
#include <tui.h>
#include <ctype.h>

static char *buf;
static size_t len;
static size_t *ls;                 /* start offset of every line */
static int nl, rows, cols, top, left, kfd;
static char pat[128];
static const char *title;

static int line_len(int i) {       /* without the newline */
    size_t e = i + 1 < nl ? ls[i + 1] - 1 : len;
    if (i + 1 == nl && e > ls[i] && buf[e - 1] == '\n') e--;
    if (e > ls[i] && buf[e - 1] == '\r') e--;
    return e - ls[i];
}

static int find_in(const char *s, int n, const char *p, int from, int icase) {
    int pl = strlen(p);
    for (int i = from; i + pl <= n; i++) {
        int k = 0;
        while (k < pl && (icase ? tolower((unsigned char)s[i + k]) == tolower((unsigned char)p[k]) : s[i + k] == p[k])) k++;
        if (k == pl) return i;
    }
    return -1;
}

static int smartcase(void) {       /* case-insensitive unless the pattern has an upper-case letter */
    for (const char *p = pat; *p; p++) if (isupper((unsigned char)*p)) return 0;
    return 1;
}

static void draw(struct sb *o, const char *msg) {
    int body = rows - 1, ic = pat[0] ? smartcase() : 0;
    sb_puts(o, "\x1b[?25l\x1b[H");
    for (int r = 0; r < body; r++) {
        int i = top + r;
        sb_puts(o, "\x1b[0m");
        if (i < nl) {
            const char *s = buf + ls[i];
            int n = line_len(i), col = 0, inm = 0, mend = -1;
            for (int k = 0; k < n && col < left + cols; k++) {
                if (pat[0] && k >= mend && find_in(s + k, n - k, pat, 0, ic) == 0) mend = k + strlen(pat);
                int hl = k < mend;
                if (hl != inm) { sb_puts(o, hl ? "\x1b[7m" : "\x1b[0m"); inm = hl; }
                unsigned char c = s[k];
                int w = c == '\t' ? 8 - col % 8 : 1;
                for (int j = 0; j < w; j++, col++)
                    if (col >= left && col < left + cols) sb_put(o, c == '\t' ? " " : (c < 32 || c == 127 ? "?" : (const char *)&s[k]), 1);
            }
        } else sb_puts(o, "\x1b[2m~");
        sb_puts(o, "\x1b[K\r\n");
    }
    int last = top + body < nl ? top + body : nl;
    char st[200];
    if (msg) snprintf(st, sizeof st, "%s", msg);
    else snprintf(st, sizeof st, " %s  lines %d-%d/%d  %d%%  (q quit, / search, space/b page)", title,
                  nl ? top + 1 : 0, last, nl, nl ? last * 100 / nl : 100);
    sb_printf(o, "\x1b[0m\x1b[7m%-*.*s\x1b[0m", cols, cols, st);
    sb_flush(o);
}

static int prompt(struct sb *o, const char *p, char *out, int max) {
    int n = 0;
    for (;;) {
        char t[200];
        snprintf(t, sizeof t, "%s%.*s", p, n, out);
        sb_printf(o, "\x1b[%d;1H\x1b[0m\x1b[7m%-*.*s\x1b[0m\x1b[%d;%dH\x1b[?25h", rows, cols, cols, t, rows, (int)strlen(t) + 1 > cols ? cols : (int)strlen(t) + 1);
        sb_flush(o);
        int k = tui_key(kfd, -1);
        if (k == 10) { out[n] = 0; return n; }
        if (k == 27 || k == CTRL('c') || k == TK_EOF) return -1;
        if (k == 8) { if (n) n--; else return -1; }
        else if (k >= 32 && k < 127 && n < max - 1) out[n++] = k;
    }
}

static void search(int dir, int from, struct sb *o, char *msg) {
    int ic = smartcase(), n = nl;
    for (int d = 0; d < n; d++) {
        int i = ((from + dir * d) % n + n) % n;
        if (find_in(buf + ls[i], line_len(i), pat, 0, ic) >= 0) {
            top = i - (rows - 1) / 3;
            if (top < 0) top = 0;
            return;
        }
    }
    snprintf(msg, 120, " Pattern not found: %s", pat);
}

int main(int argc, char **argv) {
    int fd = 0;
    title = "(stdin)";
    if (argc > 1) {
        title = argv[1];
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) { WARN("less", argv[1]); return 1; }
        if (is_dir(argv[1])) { fprintf(stderr, "less: %s is a directory\n", argv[1]); return 1; }
    } else if (isatty(0)) { fprintf(stderr, "Missing filename (usage: less file)\n"); return 1; }
    buf = readall(fd, &len);
    if (!buf) { WARN("less", title); return 1; }
    ls = malloc(sizeof *ls * (len + 2));
    for (size_t i = 0; i <= len; i++) {
        if (i == 0 || (buf[i - 1] == '\n' && i < len)) ls[nl++] = i;
    }
    if (!len) nl = 0;
    if (!isatty(1)) { write_all(1, buf, len); return 0; }       /* piped output: act like cat */
    kfd = open("/dev/tty", O_RDWR);
    if (kfd < 0) kfd = 0;
    tui_enter(kfd);
    atexit(tui_leave);
    struct sb o = { 0 };
    char msg[120] = "";
    for (;;) {
        int r0 = rows, c0 = cols;
        tui_size(1, &rows, &cols);
        int body = rows - 1, maxtop = nl > body ? nl - body : 0;
        if (rows != r0 || cols != c0) sb_puts(&o, "\x1b[2J");
        if (top > maxtop) top = maxtop;
        if (top < 0) top = 0;
        draw(&o, msg[0] ? msg : NULL);
        msg[0] = 0;
        int k = tui_key(kfd, 300);
        switch (k) {
        case TK_TIMEOUT: continue;
        case TK_EOF: case 'q': case 'Q': case 27: case CTRL('c'): return 0;
        case 'j': case KEY_DOWN: case 10: case CTRL('n'): top++; break;
        case 'k': case KEY_UP: case CTRL('p'): top--; break;
        case ' ': case 'f': case KEY_PGDN: case CTRL('f'): top += body - 1; break;
        case 'b': case KEY_PGUP: case CTRL('b'): top -= body - 1; break;
        case 'd': top += body / 2; break;
        case 'u': top -= body / 2; break;
        case 'g': case KEY_HOME: case '<': top = 0; break;
        case 'G': case KEY_END: case '>': top = maxtop; break;
        case KEY_RIGHT: left += 8; break;
        case KEY_LEFT: left = left > 8 ? left - 8 : 0; break;
        case '/': {
            char t[128] = "";
            int n = prompt(&o, "/", t, sizeof t);
            if (n > 0) strlcpy(pat, t, sizeof pat);
            if (n >= 0 && pat[0]) search(1, top + 1, &o, msg);
            break;
        }
        case 'n': if (pat[0]) search(1, top + 1, &o, msg); break;
        case 'N': if (pat[0]) search(-1, top - 1, &o, msg); break;
        }
    }
}
