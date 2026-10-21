/* edit [file] : small nano-like editor.
 *   ^S save   ^Q quit   ^F find (Enter on empty = repeat)   ^G goto line   ^K cut line   ^U paste   ^A/^E line start/end
 *   arrows, Home/End, PgUp/PgDn, Del, Tab (spaces).  C syntax colours for *.c / *.h. */
#include <tui.h>
#include <ctype.h>

struct line { char *s; int n, cap; };

static struct line *L;
static int nl, capl, cx, cy, rowoff, coloff, rows, cols, dirty, kfd, syntax;
static int mlen, mrow, mcol;                  /* current find match (inverse), mlen 0 = none */
static char fname[256], msg[160], pat[64], *cutbuf;
static int cutlen;

/* ---- buffer ---- */
static void row_reserve(struct line *l, int n) {
    if (n + 1 > l->cap) { l->cap = (n + 1) * 2; l->s = realloc(l->s, l->cap); if (!l->s) { tui_leave(); fputs("edit: out of memory\n", stderr); _exit(1); } }
}

static void row_insert_line(int at, const char *s, int n) {
    if (nl == capl) { capl = capl ? capl * 2 : 64; L = realloc(L, capl * sizeof *L); if (!L) { tui_leave(); fputs("edit: out of memory\n", stderr); _exit(1); } }
    memmove(&L[at + 1], &L[at], (nl - at) * sizeof *L);
    nl++;
    L[at] = (struct line){ 0, 0, 0 };
    row_reserve(&L[at], n);
    memcpy(L[at].s, s, n);
    L[at].n = n;
}

static void row_delete_line(int at) {
    free(L[at].s);
    memmove(&L[at], &L[at + 1], (nl - at - 1) * sizeof *L);
    nl--;
}

static void row_insert(struct line *l, int at, const char *s, int n) {
    row_reserve(l, l->n + n);
    memmove(l->s + at + n, l->s + at, l->n - at);
    memcpy(l->s + at, s, n);
    l->n += n;
}

static void row_remove(struct line *l, int at, int n) {
    memmove(l->s + at, l->s + at + n, l->n - at - n);
    l->n -= n;
}

#define CONT(c) (((unsigned char)(c) & 0xC0) == 0x80)       /* UTF-8 continuation byte */
static int prev_ch(const struct line *l, int x) { do x--; while (x > 0 && CONT(l->s[x])); return x; }
static int next_ch(const struct line *l, int x) { do x++; while (x < l->n && CONT(l->s[x])); return x; }

static int to_rx(const struct line *l, int x) {              /* byte index -> display column */
    int rx = 0;
    for (int i = 0; i < x && i < l->n; i++) {
        if (l->s[i] == '\t') rx += 4 - rx % 4;
        else if (!CONT(l->s[i])) rx++;
    }
    return rx;
}

static void load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) { snprintf(msg, sizeof msg, "New file: %s", path); return; }
        snprintf(msg, sizeof msg, "%s: %s", path, strerror(errno));
        return;
    }
    size_t len;
    char *b = readall(fd, &len);
    close(fd);
    if (!b) { snprintf(msg, sizeof msg, "%s: %s", path, strerror(errno)); return; }
    for (size_t i = 0; i < len;) {
        char *e = memchr(b + i, '\n', len - i);
        size_t n = e ? (size_t)(e - (b + i)) : len - i;
        row_insert_line(nl, b + i, n);
        i += n + 1;
    }
    free(b);
    snprintf(msg, sizeof msg, "Read %d lines from %s", nl, path);
}

static int save(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(msg, sizeof msg, "Cannot save %s: %s", path, strerror(errno)); return -1; }
    int bad = 0;
    for (int i = 0; i < nl && !bad; i++)
        if (write_all(fd, L[i].s, L[i].n) < 0 || write_all(fd, "\n", 1) < 0) bad = 1;
    if (close(fd) < 0) bad = 1;
    if (bad) { snprintf(msg, sizeof msg, "Write error on %s: %s", path, strerror(errno)); return -1; }
    snprintf(msg, sizeof msg, "Wrote %d lines to %s", nl, path);
    dirty = 0;
    return 0;
}

/* ---- C highlighting ---- */
enum { H_NONE, H_KW, H_TYPE, H_STR, H_COM, H_NUM, H_PRE };
static const char *const kws[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline", "register", 0 };
static const char *const types[] = { "int", "char", "short", "long", "unsigned", "signed", "float", "double", "void", "bool", "size_t",
    "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "FILE", "off_t", "pid_t", 0 };
static const char *const consts[] = { "NULL", "true", "false", "EOF", 0 };

static int in_list(const char *const *l, const char *s, int n) {
    for (; *l; l++) if ((int)strlen(*l) == n && !memcmp(*l, s, n)) return 1;
    return 0;
}

static void hl_line(const struct line *l, int *inc, char *hl) {
    const char *s = l->s;
    int i = 0, n = l->n;
    memset(hl, 0, n + 1);
    while (i < n && *inc) {
        hl[i] = H_COM;
        if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { hl[i + 1] = H_COM; i += 2; *inc = 0; break; }
        i++;
    }
    int j = i;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    int pre = j < n && s[j] == '#' && !*inc;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { memset(hl + i, H_COM, n - i); return; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            hl[i] = hl[i + 1] = H_COM;
            i += 2;
            *inc = 1;
            while (i < n) {
                hl[i] = H_COM;
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { hl[i + 1] = H_COM; i += 2; *inc = 0; break; }
                i++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            hl[i++] = H_STR;
            while (i < n) {
                hl[i] = H_STR;
                if (s[i] == '\\' && i + 1 < n) { hl[i + 1] = H_STR; i += 2; continue; }
                if (s[i++] == c) break;
            }
            continue;
        }
        if (isdigit((unsigned char)c) && (i == 0 || (!isalnum((unsigned char)s[i - 1]) && s[i - 1] != '_'))) {
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.')) hl[i++] = H_NUM;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int st = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            int k = in_list(kws, s + st, i - st) ? H_KW : in_list(types, s + st, i - st) ? H_TYPE : in_list(consts, s + st, i - st) ? H_NUM : 0;
            if (k) memset(hl + st, k, i - st);
            continue;
        }
        i++;
    }
    if (pre) for (int k = 0; k < n; k++) if (!hl[k] && s[k] != ' ' && s[k] != '\t') hl[k] = H_PRE;
}

static const char *const hcol[] = { "\x1b[39;22;23m", "\x1b[38;2;198;120;221m", "\x1b[38;2;86;182;194m", "\x1b[38;2;152;195;121m",
                                    "\x1b[38;2;110;118;140m", "\x1b[38;2;229;192;123m", "\x1b[38;2;224;108;117m" };

/* ---- screen ---- */
static int digits(int n) { int d = 1; while (n >= 10) { n /= 10; d++; } return d; }

static void scroll(int gw) {
    int tr = rows - 3, tw = cols - gw;
    int rx = to_rx(&L[cy], cx);
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + tr) rowoff = cy - tr + 1;
    if (rx < coloff) coloff = rx;
    if (rx >= coloff + tw) coloff = rx - tw + 1;
}

static char *hlbuf;
static int hlcap;

static void draw(struct sb *o) {
    int gw = digits(nl) + 2, tr = rows - 3, tw = cols - gw;
    if (tw < 1) tw = 1;
    scroll(gw);
    sb_puts(o, "\x1b[?25l\x1b[H\x1b[0m");
    char pos[40];
    snprintf(pos, sizeof pos, "Ln %d, Col %d ", cy + 1, to_rx(&L[cy], cx) + 1);
    char title[300];
    snprintf(title, sizeof title, " VibeEdit  %s%s", fname[0] ? fname : "[No Name]", dirty ? " [modified]" : "");
    int tl = strlen(title), pl = strlen(pos);
    sb_printf(o, "\x1b[1;48;2;60;50;110;38;2;235;230;255m%s%*s%s\x1b[0m\x1b[K", title, cols - tl - pl > 0 ? cols - tl - pl : 0, "", pos);
    int inc = 0;
    if (syntax) {
        for (int i = 0; i < rowoff; i++) {
            if (L[i].n + 1 > hlcap) { hlcap = (L[i].n + 1) * 2; hlbuf = realloc(hlbuf, hlcap); }
            hl_line(&L[i], &inc, hlbuf);
        }
    }
    for (int r = 0; r < tr; r++) {
        int i = rowoff + r;
        sb_printf(o, "\x1b[%d;1H\x1b[0m", r + 2);
        if (i >= nl) { sb_puts(o, "\x1b[38;2;70;76;100m~\x1b[0m\x1b[K"); continue; }
        sb_printf(o, "\x1b[38;2;90;96;125m%*d \x1b[0m", gw - 1, i + 1);
        struct line *l = &L[i];
        if (syntax) {
            if (l->n + 1 > hlcap) { hlcap = (l->n + 1) * 2; hlbuf = realloc(hlbuf, hlcap); }
            hl_line(l, &inc, hlbuf);
        }
        int col = 0, cur = 0, inv = 0, shown = 0;
        for (int k = 0; k < l->n && col < coloff + tw; k++) {
            unsigned char c = l->s[k];
            int hv = mlen && i == mrow && k >= mcol && k < mcol + mlen;
            if (hv != inv) { sb_puts(o, hv ? "\x1b[7m" : "\x1b[27m"); inv = hv; }
            int cls = syntax ? hlbuf[k] : 0;
            if (cls != cur) { sb_puts(o, hcol[cls]); cur = cls; }
            if (c == '\t') {
                int w = 4 - col % 4;
                for (int j = 0; j < w; j++, col++) if (col >= coloff && col < coloff + tw) sb_put(o, " ", 1);
            } else if (CONT(c)) {
                if (shown) sb_put(o, (const char *)&c, 1);       /* tail of a visible multi-byte char */
            } else {
                shown = col >= coloff && col < coloff + tw;
                if (shown) sb_put(o, c < 32 || c == 127 ? "?" : (const char *)&l->s[k], 1);
                col++;
            }
        }
        sb_puts(o, "\x1b[0m\x1b[K");
    }
    sb_printf(o, "\x1b[%d;1H\x1b[0m\x1b[K", rows - 1);
    if (msg[0]) sb_printf(o, "\x1b[1;38;2;255;214;130m %.*s\x1b[0m", cols - 2, msg);
    static const char *const help[][2] = { { "^S", "Save" }, { "^Q", "Quit" }, { "^F", "Find" }, { "^G", "Goto" }, { "^K", "Cut" }, { "^U", "Paste" } };
    sb_printf(o, "\x1b[%d;1H\x1b[0m", rows);
    for (unsigned h = 0; h < sizeof help / sizeof *help; h++)
        sb_printf(o, "\x1b[7m%s\x1b[27m %s  ", help[h][0], help[h][1]);
    sb_puts(o, "\x1b[K");
    sb_printf(o, "\x1b[%d;%dH\x1b[?25h", cy - rowoff + 2, gw + to_rx(&L[cy], cx) - coloff + 1);
    sb_flush(o);
}

/* one-line prompt on the message row; returns length or -1 on ESC */
static int prompt(struct sb *o, const char *label, char *out, int max) {
    int n = (int)strlen(out);
    for (;;) {
        char t[220];
        snprintf(t, sizeof t, "%s%s", label, out);
        int l = (int)strlen(t);
        sb_printf(o, "\x1b[%d;1H\x1b[0m\x1b[K\x1b[1;38;2;255;214;130m %.*s\x1b[0m\x1b[%d;%dH\x1b[?25h", rows - 1, cols - 2, t, rows - 1, l + 2 > cols ? cols : l + 2);
        sb_flush(o);
        int k = tui_key(kfd, -1);
        if (k == 10) return n;
        if (k == 27 || k == CTRL('c') || k == CTRL('q') || k == TK_EOF) return -1;
        if (k == 8) { while (n > 0 && CONT(out[n - 1])) out[--n] = 0; if (n) out[--n] = 0; }
        else if (k >= 32 && k < 256 && k != 127 && n < max - 1) { out[n++] = k; out[n] = 0; }
    }
}

static int find_from(int y, int x, int wrap) {                /* search forward from (y,x) */
    int pl = strlen(pat);
    for (int d = 0; d <= nl; d++) {
        int i = (y + d) % nl;
        if (!wrap && y + d >= nl) return 0;
        int from = d == 0 ? x : 0;
        for (int k = from; k + pl <= L[i].n; k++)
            if (!memcmp(L[i].s + k, pat, pl)) { cy = i; cx = k; mrow = i; mcol = k; mlen = pl; return d && y + d >= nl ? 2 : 1; }
    }
    return 0;
}

static void insert_str(const char *s, int n) {
    row_insert(&L[cy], cx, s, n);
    cx += n;
    dirty = 1;
}

static void newline(void) {
    struct line *l = &L[cy];
    int ind = 0;
    while (ind < l->n && ind < cx && (l->s[ind] == ' ' || l->s[ind] == '\t')) ind++;
    char *indent = malloc(ind + 1);
    memcpy(indent, l->s, ind);
    row_insert_line(cy + 1, l->s + cx, l->n - cx);
    L[cy].n = cx;
    cy++;
    cx = 0;
    row_insert(&L[cy], 0, indent, ind);
    cx = ind;
    free(indent);
    dirty = 1;
}

static void clamp(void) {
    if (cy < 0) cy = 0;
    if (cy >= nl) cy = nl - 1;
    if (cx > L[cy].n) cx = L[cy].n;
    if (cx < 0) cx = 0;
}

int main(int argc, char **argv) {
    if (!isatty(1)) { fprintf(stderr, "edit: stdout is not a terminal\n"); return 1; }
    if (argc > 1) {
        strlcpy(fname, argv[1], sizeof fname);
        load(fname);
        size_t fl = strlen(fname);
        syntax = fl > 2 && (!strcmp(fname + fl - 2, ".c") || !strcmp(fname + fl - 2, ".h"));
    }
    if (!nl) row_insert_line(0, "", 0);
    kfd = open("/dev/tty", O_RDWR);
    if (kfd < 0) kfd = 0;
    tui_enter(kfd);
    atexit(tui_leave);
    struct sb o = { 0 };
    tui_size(1, &rows, &cols);
    if (rows < 6) rows = 6;
    int redraw = 1;
    for (;;) {
        if (redraw) draw(&o);
        redraw = 1;
        int k = tui_key(kfd, 300);
        if (k == TK_TIMEOUT) {                      /* idle: only a resize needs a repaint */
            int r, c;
            tui_size(1, &r, &c);
            redraw = r != rows || c != cols;
            if (redraw) { rows = r < 6 ? 6 : r; cols = c; sb_puts(&o, "\x1b[2J"); }
            continue;
        }
        msg[0] = 0;
        if (k != CTRL('f')) mlen = 0;
        struct line *l = &L[cy];
        switch (k) {
        case TK_EOF: tui_leave(); return 1;
        case KEY_UP: cy--; break;
        case KEY_DOWN: cy++; break;
        case KEY_LEFT: if (cx > 0) cx = prev_ch(l, cx); else if (cy > 0) { cy--; cx = L[cy].n; } break;
        case KEY_RIGHT: if (cx < l->n) cx = next_ch(l, cx); else if (cy + 1 < nl) { cy++; cx = 0; } break;
        case KEY_HOME: case CTRL('a'): cx = 0; break;
        case KEY_END: case CTRL('e'): cx = l->n; break;
        case KEY_PGUP: cy -= rows - 3; break;
        case KEY_PGDN: cy += rows - 3; break;
        case 10: newline(); break;
        case '\t': { int sp = 4 - to_rx(l, cx) % 4; insert_str("    ", sp); break; }
        case 8:
            if (cx > 0) { int p = prev_ch(l, cx); row_remove(l, p, cx - p); cx = p; dirty = 1; }
            else if (cy > 0) {
                int px = L[cy - 1].n;
                row_insert(&L[cy - 1], px, l->s, l->n);
                row_delete_line(cy);
                cy--; cx = px; dirty = 1;
            }
            break;
        case TK_DEL:
            if (cx < l->n) { row_remove(l, cx, next_ch(l, cx) - cx); dirty = 1; }
            else if (cy + 1 < nl) { row_insert(l, l->n, L[cy + 1].s, L[cy + 1].n); row_delete_line(cy + 1); dirty = 1; }
            break;
        case CTRL('s'):
            if (!fname[0]) {
                char t[256] = "";
                if (prompt(&o, "Save as: ", t, sizeof t) <= 0) { snprintf(msg, sizeof msg, "Save cancelled"); break; }
                strlcpy(fname, t, sizeof fname);
                size_t fl = strlen(fname);
                syntax = fl > 2 && (!strcmp(fname + fl - 2, ".c") || !strcmp(fname + fl - 2, ".h"));
            }
            save(fname);
            break;
        case CTRL('q'):
            if (dirty) {
                snprintf(msg, sizeof msg, "Unsaved changes! y = save and quit, n = discard, any other key = cancel");
                draw(&o);
                int a = tui_key(kfd, -1);
                msg[0] = 0;
                if (a == 'n' || a == 'N') { tui_leave(); return 0; }
                if (a == 'y' || a == 'Y') {
                    if (!fname[0]) {
                        char t[256] = "";
                        if (prompt(&o, "Save as: ", t, sizeof t) <= 0) break;
                        strlcpy(fname, t, sizeof fname);
                    }
                    if (save(fname) == 0) { tui_leave(); return 0; }
                } else snprintf(msg, sizeof msg, "Quit cancelled");
            } else { tui_leave(); return 0; }
            break;
        case CTRL('f'): {
            char t[64] = "";
            int n = prompt(&o, pat[0] ? "Find (Enter = repeat): " : "Find: ", t, sizeof t);
            if (n < 0) break;
            if (n > 0) strlcpy(pat, t, sizeof pat);
            if (!pat[0]) break;
            int r = find_from(cy, cx + (mlen ? 1 : 0), 1);
            if (!r) snprintf(msg, sizeof msg, "\"%s\" not found", pat);
            else if (r == 2) snprintf(msg, sizeof msg, "Search wrapped");
            break;
        }
        case CTRL('g'): {
            char t[16] = "";
            if (prompt(&o, "Go to line: ", t, sizeof t) > 0) { cy = atoi(t) - 1; cx = 0; }
            break;
        }
        case CTRL('k'):
            free(cutbuf);
            cutbuf = malloc(l->n + 1);
            memcpy(cutbuf, l->s, l->n);
            cutlen = l->n;
            if (nl == 1) l->n = 0; else row_delete_line(cy);
            cx = 0;
            dirty = 1;
            break;
        case CTRL('u'):
            if (cutbuf) { row_insert_line(cy, cutbuf, cutlen); cy++; cx = 0; dirty = 1; }
            else snprintf(msg, sizeof msg, "Nothing to paste (^K cuts a line)");
            break;
        default:
            if (k >= 32 && k < 256 && k != 127) { char c = k; insert_str(&c, 1); }
        }
        clamp();
    }
}
