/* notepad - plain text editor: caret, selection, clipboard, line numbers, open/save */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <syscall.h>

#define MAX_FILE (1 << 20)
#define TAB 4

/* ---- document ---- */
static char *buf;
static int len, cap;
static int cur, anchor;            /* caret and the other end of the selection */
static int want_col = -1;          /* remembered visual column for up/down */
static bool modified;
static char path[PATH_MAX];        /* "" = untitled */

/* line index, rebuilt after every edit */
static int *lines, nlines, lines_cap;
static bool lines_dirty = true;

/* view */
static int top, hx;                /* first visible line, horizontal pixel offset */
static uint32_t last_act;          /* uptime of the last caret movement, for blink */
static struct ui ui;

enum { M_EDIT, M_OPEN, M_SAVEAS, M_CLOSE, M_GOTO };
static int mode = M_EDIT;
static bool closing;               /* Save pressed in the close dialog: exit once saved */
static char prompt[PATH_MAX];

static int CW, LH;

static void say(bool bad, const char *fmt, const char *arg) {
    char m[96];
    snprintf(m, sizeof m, fmt, arg);
    ui_toast(&ui, m, NULL);
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void reindex(void) {
    if (!lines_dirty) return;
    lines_dirty = false;
    nlines = 1;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') nlines++;
    if (nlines > lines_cap) { lines_cap = nlines + 256; lines = realloc(lines, lines_cap * sizeof *lines); }
    int n = 0;
    lines[n++] = 0;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') lines[n++] = i + 1;
}

static int line_of(int off) {
    reindex();
    int lo = 0, hi = nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (lines[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}
static int line_end(int l) { reindex(); return l + 1 < nlines ? lines[l + 1] - 1 : len; }

static int vcol(int l, int off) {
    int v = 0;
    for (int i = lines[l]; i < off; i++) v = buf[i] == '\t' ? (v / TAB + 1) * TAB : v + 1;
    return v;
}
/* offset in line l whose column is the first at or after visual column vc */
static int off_at_vcol(int l, int vc) {
    int v = 0, e = line_end(l);
    for (int i = lines[l]; i < e; i++) {
        if (vc <= v) return i;
        v = buf[i] == '\t' ? (v / TAB + 1) * TAB : v + 1;
    }
    return e;
}

static void ensure(int extra) {
    if (len + extra + 1 <= cap) return;
    cap = (len + extra + 1) * 2;
    buf = realloc(buf, cap);
}

static void sel_range(int *lo, int *hi) { *lo = cur < anchor ? cur : anchor; *hi = cur < anchor ? anchor : cur; }

/* undo: every change goes through rec(); edits of one command (or a run of typing/backspacing) share a group */
struct edit { int pos, ndel, nins, grp; char *del, *ins; };
static struct edit *undo, *redo;
static int nundo, ucap, nredo, rcap, grp, grp_ctr, lastop;
static int save_at;                            /* nundo when the file was last saved/loaded; -1 = no longer reachable by undo/redo */
static unsigned ver;                           /* bumped on every edit (find stats cache) */

static void free_edits(struct edit *e, int n) { for (int i = 0; i < n; i++) { free(e[i].del); free(e[i].ins); } }
static void clear_history(void) { free_edits(undo, nundo); free_edits(redo, nredo); nundo = nredo = 0; lastop = 0; save_at = 0; }
static void push_edit(struct edit **a, int *n, int *c, struct edit e) {
    if (*n == *c) { *c = *c ? *c * 2 : 64; *a = realloc(*a, *c * sizeof **a); }
    (*a)[(*n)++] = e;
}
static void begin_op(int op) { if (op == 0 || lastop != op) grp = ++grp_ctr; lastop = op; }

static void raw_replace(int pos, int ndel, const char *ins, int nins) {
    ensure(nins);
    memmove(buf + pos + nins, buf + pos + ndel, len - pos - ndel);
    memcpy(buf + pos, ins, nins);
    len += nins - ndel;
    buf[len] = 0;
    modified = lines_dirty = true;
    ver++;
}

static void rec(int pos, int ndel, const char *ins, int nins) {
    if (save_at > nundo) save_at = -1;             /* redo history holding the saved state is dropped */
    free_edits(redo, nredo);
    nredo = 0;
    struct edit e = { pos, ndel, nins, grp, malloc(ndel + 1), malloc(nins + 1) };
    memcpy(e.del, buf + pos, ndel);
    memcpy(e.ins, ins, nins);
    if (nundo >= 4000) { free_edits(undo, 100); memmove(undo, undo + 100, (nundo - 100) * sizeof *undo); nundo -= 100; if (save_at >= 0) save_at = save_at >= 100 ? save_at - 100 : -1; }
    push_edit(&undo, &nundo, &ucap, e);
    raw_replace(pos, ndel, ins, nins);
    modified = nundo != save_at;
    cur = anchor = pos + nins;
}

static void undo_redo(bool re) {
    struct edit **from = re ? &redo : &undo, **to = re ? &undo : &redo;
    int *nf = re ? &nredo : &nundo, *nt = re ? &nundo : &nredo, *ct = re ? &ucap : &rcap;
    if (!*nf) return;
    int g = (*from)[*nf - 1].grp;
    while (*nf && (*from)[*nf - 1].grp == g) {
        struct edit e = (*from)[--*nf];
        if (re) { raw_replace(e.pos, e.ndel, e.ins, e.nins); cur = e.pos + e.nins; }
        else { raw_replace(e.pos, e.nins, e.del, e.ndel); cur = e.pos + e.ndel; }
        push_edit(to, nt, ct, e);
    }
    anchor = cur;
    lastop = 0;
    modified = nundo != save_at;
}

static void del_range(int lo, int hi) { if (hi > lo) rec(lo, hi - lo, "", 0); }

static void insert(const char *s, int n) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (len - (hi - lo) + n > MAX_FILE) { say(true, "%s", "The file is too large (1 MiB at most)"); return; }
    rec(lo, hi - lo, s, n);
}

static void copy_sel(void) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (hi > lo) gui_clip_set(buf + lo, hi - lo);
}

/* ---- find / replace (Ctrl+F / Ctrl+H, F3, Shift+F3) ---- */
static bool find_open, replace_open, find_case;
static char fq[128], rq[128];
static unsigned fc_ver = ~0u; static int fc_total, fc_index; static char fc_q[128]; static bool fc_case;

static bool match_at(int pos, int n) {
    for (int i = 0; i < n; i++) {
        int a = (unsigned char)buf[pos + i], b = (unsigned char)fq[i];
        if (find_case ? a != b : tolower(a) != tolower(b)) return false;
    }
    return true;
}

static void find_next(bool back) {
    int n = strlen(fq), lo, hi;
    if (!n) return;
    sel_range(&lo, &hi);
    int start = back ? lo - 1 : hi;
    for (int k = 0; k <= len; k++) {
        int pos = back ? ((start - k) % (len + 1) + len + 1) % (len + 1) : (start + k) % (len + 1);
        if (pos + n <= len && match_at(pos, n)) { anchor = pos; cur = pos + n; want_col = -1; last_act = uptime_ms(); return; }
    }
    say(true, "No matches for \u201c%s\u201d", fq);
}

static void find_stats(void) {             /* "3 of 12": the total is rescanned only when text or query changed */
    int n = strlen(fq), lo, hi;
    if (fc_ver != ver || fc_case != find_case || strcmp(fc_q, fq)) {
        fc_ver = ver; fc_case = find_case; snprintf(fc_q, sizeof fc_q, "%s", fq);
        fc_total = 0;
        for (int p = 0; n && p + n <= len; p++) if (match_at(p, n)) fc_total++;
    }
    sel_range(&lo, &hi);
    fc_index = 0;
    if (n && hi - lo == n && match_at(lo, n)) {
        int k = 0;
        for (int p = 0; p + n <= lo; p++) if (match_at(p, n)) k++;
        fc_index = k + 1;
    }
}

static void replace_one(void) {
    int n = strlen(fq), lo, hi;
    if (!n) return;
    sel_range(&lo, &hi);
    if (hi - lo == n && match_at(lo, n)) { begin_op(0); rec(lo, n, rq, strlen(rq)); }
    find_next(false);
}

static void replace_all(void) {
    int n = strlen(fq), cnt = 0;
    if (!n) return;
    int *pos = malloc((len / n + 1) * sizeof *pos);
    for (int p = 0; p + n <= len; ) if (match_at(p, n)) { pos[cnt++] = p; p += n; } else p++;
    begin_op(0);
    for (int i = cnt - 1; i >= 0; i--) rec(pos[i], n, rq, strlen(rq));    /* back to front keeps offsets valid */
    free(pos);
    char t[40];
    snprintf(t, sizeof t, "%d occurrence%s", cnt, cnt == 1 ? "" : "s");
    say(false, "Replaced %s", t);
}

static void new_doc(void) {
    len = cur = anchor = 0;
    buf[0] = 0;
    modified = false;
    lines_dirty = true;
    path[0] = 0;
    top = hx = 0;
    clear_history();
}

static bool load(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { say(true, "Couldn't open \u201c%s\u201d", base(p)); return false; }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); say(true, "\u201c%s\u201d is a folder", base(p)); return false; }
    len = 0;
    for (;;) {
        ensure(4096);
        int n = read(fd, buf + len, 4096);
        if (n <= 0) break;
        len += n;
        if (len >= MAX_FILE) { say(true, "%s", "Opened the first 1 MiB only"); break; }
    }
    close(fd);
    { int o = 0; for (int i = 0; i < len; i++) if (buf[i] != '\r') buf[o++] = buf[i]; len = o; }   /* CRLF -> LF in one pass */
    buf[len] = 0;
    cur = anchor = 0;
    top = hx = 0;
    clear_history();
    modified = false;
    lines_dirty = true;
    snprintf(path, sizeof path, "%s", p);
    return true;
}

static bool save(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { say(true, "Couldn't save \u201c%s\u201d", base(p)); return false; }
    int off = 0;
    while (off < len) {
        int n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); say(true, "Couldn't save \u201c%s\u201d", base(p)); return false; }
        off += n;
    }
    close(fd);
    snprintf(path, sizeof path, "%s", p);
    modified = false;
    save_at = nundo;
    say(false, "Saved \u201c%s\u201d", base(p));
    return true;
}

static void open_prompt(int m) {
    mode = m;
    snprintf(prompt, sizeof prompt, "%s", m == M_SAVEAS ? (path[0] ? path : "/home/untitled.txt") : (path[0] ? path : "/home/"));
}

static void do_save(void) {
    if (path[0]) save(path); else open_prompt(M_SAVEAS);
}

static bool isword(int c) { return isalnum(c) || c == '_'; }

static void move_to(int off, bool extend, bool keep_col) {
    if (off < 0) off = 0;
    if (off > len) off = len;
    cur = off;
    if (!extend) anchor = cur;
    if (!keep_col) want_col = -1;
    last_act = uptime_ms();
    lastop = 0;                                   /* a caret move ends the typing run for undo */
}

static void handle_key(const struct gui_event *k, int vis_lines) {
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    int l = line_of(cur);
    if (ctrl) {
        begin_op(0);
        int key = k->key < 256 ? tolower(k->key) : k->key;
        switch (key) {
        case 'z': undo_redo(shift); break;
        case 'y': undo_redo(true); break;
        case 'a': anchor = 0; cur = len; break;
        case 'c': copy_sel(); break;
        case 'x': copy_sel(); { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); } break;
        case 'v': { int n; char *c = gui_clip_get(&n); if (c) insert(c, n); free(c); break; }
        case KEY_LEFT: { int i = cur; while (i > 0 && !isword(buf[i - 1])) i--; while (i > 0 && isword(buf[i - 1])) i--; move_to(i, shift, false); break; }
        case KEY_RIGHT: { int i = cur; while (i < len && !isword(buf[i])) i++; while (i < len && isword(buf[i])) i++; move_to(i, shift, false); break; }
        case KEY_BACKSPACE: { int i = cur; while (i > 0 && !isword(buf[i - 1])) i--; while (i > 0 && isword(buf[i - 1])) i--; if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); } else del_range(i, cur); break; }
        case KEY_DELETE: { int i = cur; while (i < len && !isword(buf[i])) i++; while (i < len && isword(buf[i])) i++; if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); } else del_range(cur, i); break; }
        case KEY_HOME: move_to(0, shift, false); break;
        case KEY_END: move_to(len, shift, false); break;
        }
        last_act = uptime_ms();
        return;
    }
    switch (k->key) {
    case KEY_LEFT:
        if (cur != anchor && !shift) move_to(cur < anchor ? cur : anchor, false, false);
        else move_to(cur - 1, shift, false);
        return;
    case KEY_RIGHT:
        if (cur != anchor && !shift) move_to(cur > anchor ? cur : anchor, false, false);
        else move_to(cur + 1, shift, false);
        return;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int d = k->key == KEY_UP ? -1 : k->key == KEY_DOWN ? 1 : k->key == KEY_PGUP ? -vis_lines : vis_lines;
        if (want_col < 0) want_col = vcol(l, cur);
        int nl = l + d;
        if (nl < 0) { move_to(0, shift, false); return; }
        if (nl >= nlines) { move_to(len, shift, false); return; }
        move_to(off_at_vcol(nl, want_col), shift, true);
        return;
    }
    case KEY_HOME: {
        int s = lines[l], i = s;
        while (i < line_end(l) && (buf[i] == ' ' || buf[i] == '\t')) i++;
        move_to(cur == i ? s : i, shift, false);     /* smart home: first non-blank, then column 0 */
        return;
    }
    case KEY_END: move_to(line_end(l), shift, false); return;
    case KEY_BACKSPACE:
        begin_op(2);
        if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); }
        else if (cur > 0) del_range(cur - 1, cur);
        want_col = -1; last_act = uptime_ms();
        return;
    case KEY_DELETE:
        begin_op(3);
        if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); }
        else if (cur < len) del_range(cur, cur + 1);
        want_col = -1; last_act = uptime_ms();
        return;
    case KEY_ENTER: {
        begin_op(0);
        char ins[128] = "\n";                         /* auto-indent */
        int n = 1, s = lines[line_of(cur < anchor ? cur : anchor)];
        while (n < 120 && (buf[s + n - 1] == ' ' || buf[s + n - 1] == '\t') && s + n - 1 < cur) { ins[n] = buf[s + n - 1]; n++; }
        insert(ins, n);
        want_col = -1; last_act = uptime_ms();
        return;
    }
    case KEY_TAB: begin_op(0); insert("    ", TAB); want_col = -1; last_act = uptime_ms(); return;
    case KEY_ESC: anchor = cur; return;
    }
    if (k->ch >= 32 && k->ch < 127 && !(k->mods & MOD_ALT)) {
        char ch = k->ch;
        begin_op(1);
        insert(&ch, 1);
        want_col = -1;
        last_act = uptime_ms();
    }
}

static void update_title(struct gui_window *w) {
    static char last[96];
    char t[96];
    snprintf(t, sizeof t, "%s%s \u2014 Text Editor", modified ? "* " : "", path[0] ? base(path) : "Untitled");
    if (strcmp(t, last)) { strcpy(last, t); gui_set_title(w, t); }
}

int main(int argc, char **argv) {
    cap = 4096;
    buf = malloc(cap);
    buf[0] = 0;
    struct gui_window *w = gui_window_create("Untitled \u2014 Text Editor", 760, 520, 0);
    if (!w) { perror("notepad"); return 1; }
    gui_set_minsize(w, 440, 220);
    const struct font *mf = T_MONO;
    CW = mf->g['M' - 32].adv;
    LH = mf->height + SP_1;
    ui_init(&ui, w);
    if (argc > 1) load(argv[1]);
    last_act = uptime_ms();
    bool dragging = false, close_req = false;
    for (;;) {
        uint32_t idle = uptime_ms() - last_act;              /* blink until D_CARET_IDLE, then stay on and sleep */
        bool alive = ui_poll(&ui, idle < D_CARET_IDLE ? (int)(D_CARET - idle % D_CARET) : -1);
        if (!alive || close_req) {
            close_req = false;
            if (!modified || mode == M_CLOSE) break;
            ui.closed = false;                          /* ask before throwing edits away */
            mode = M_CLOSE;
        }
        uint32_t now = uptime_ms();
        int W = w->w, H = w->h;
        bool modal = mode != M_EDIT;
        int tbH = H_TOOLBAR, sbH = H_STATUS;
        int gutter = 0;
        reindex();
        for (int n = nlines; n; n /= 10) gutter += CW;
        gutter += 2 * SP_4;
        int ex = 0, ey = tbH, ew = W, eh = H - tbH - sbH;      /* editor area */
        int tx0 = ex + gutter + SP_2;                          /* text origin */
        int vis = eh / LH;

        /* global shortcuts, taken before the editor or a focused text box can swallow the key */
        int fbw = 400, fbx = ex + ew - fbw - SP_5 - 12, fby = ey + SP_3, fbh = (replace_open ? 2 * H_CTL + SP_3 : H_CTL) + 2 * SP_3;
        int fq_x = fbx + SP_3, fq_y = fby + SP_3, fq_w = 180, fq_h = H_CTL, rq_y = fq_y + H_CTL + SP_3;
        for (int i = 0; !modal && i < ui.nkeys; ) {
            struct gui_event gk = ui.keys[i];
            bool gc = gk.mods & MOD_CTRL, used = true;
            int key = gk.key < 128 ? tolower(gk.key) : gk.key;
            if (gk.key == KEY_F3) find_next(gk.mods & MOD_SHIFT);
            else if (gc && key == 's') do_save();
            else if (gc && key == 'o') open_prompt(M_OPEN);
            else if (gc && key == 'n') { if (!modified) new_doc(); else say(true, "%s", "Save your changes first (Ctrl+S)"); }
            else if (gc && (key == 'w' || key == 'q')) close_req = true;
            else if (gc && key == 'g') { mode = M_GOTO; prompt[0] = 0; }
            else if (gc && (key == 'f' || key == 'h')) {
                find_open = true; replace_open = key == 'h';
                int lo, hi;
                sel_range(&lo, &hi);
                if (hi > lo && hi - lo < (int)sizeof fq && !memchr(buf + lo, '\n', hi - lo)) { memcpy(fq, buf + lo, hi - lo); fq[hi - lo] = 0; }
                ui.focus = ui_id(fq_x, fq_y, fq_w, fq_h);
            }
            else if (gk.key == KEY_ESC && find_open) { find_open = false; ui.focus = 0; }
            else used = false;
            if (used) memmove(ui.keys + i, ui.keys + i + 1, (--ui.nkeys - i) * sizeof gk); else i++;
        }

        /* keys (a dialog or a focused text box takes them while it is up) */
        struct gui_event k;
        while (!modal && !ui.focus && ui_key(&ui, &k)) {
            handle_key(&k, vis);
            reindex();
            int l = line_of(cur);
            if (l < top) top = l;
            if (l >= top + vis) top = l - vis + 1;
            int cx = vcol(l, cur) * CW;
            int viewW = ew - gutter - 14;
            if (cx - hx > viewW - CW) hx = cx - viewW + CW * 4;
            if (cx < hx) hx = cx > CW * 4 ? cx - CW * 4 : 0;
        }
        reindex();

        /* mouse in the editor */
        bool in_ed = !modal && ui_hover(&ui, tx0, ey, ew - tx0, eh) && !(find_open && ui_hover(&ui, fbx, fby, fbw, fbh));
        if (in_ed && ui.scroll) top += ui.scroll * 3;
        if (ui.pressed && in_ed) {
            int l = top + (ui.my - ey - SP_2) / LH;
            if (l < 0) l = 0;
            if (l >= nlines) l = nlines - 1;
            int vc = (ui.mx - tx0 + hx + CW / 2) / CW;
            int off = off_at_vcol(l, vc < 0 ? 0 : vc);
            if (ui.dbl) {                                         /* select word */
                int a = off, b = off;
                while (a > lines[l] && isword(buf[a - 1])) a--;
                while (b < line_end(l) && isword(buf[b])) b++;
                anchor = a; cur = b;
            } else {
                cur = anchor = off;
            }
            dragging = true;
            want_col = -1;
            last_act = now;
        } else if (dragging && (ui.buttons & 1)) {
            int l = top + (ui.my - ey - SP_2) / LH;
            if (l < top) { l = top - 1; if (l < 0) l = 0; }
            if (l >= nlines) l = nlines - 1;
            int vc = (ui.mx - tx0 + hx + CW / 2) / CW;
            cur = off_at_vcol(l, vc < 0 ? 0 : vc);
            if (ui.my > ey + eh) top++;
            if (ui.my < ey && top > 0) top--;
            last_act = now;
        }
        if (ui.released || !(ui.buttons & 1)) dragging = false;
        if (top > nlines - vis) top = nlines - vis;
        if (top < 0) top = 0;

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        ui_toolbar(&ui, 0, 0, W);
        int bx = SP_3, by = (tbH - H_CTL) / 2;
        static const char *const tl[] = { "New", "Open\u2026", "Save", "Save as\u2026" };
        static const int ti[] = { ICON_FILE_NEW, ICON_FOLDER_OPEN, ICON_SAVE, UI_NOICON };
        static const char *const tips[] = { "New \u00b7 Ctrl+N", "Open \u00b7 Ctrl+O", "Save \u00b7 Ctrl+S", NULL };
        for (int i = 0; i < 4; i++) {
            int bw = ui_button_w(ti[i], tl[i]);
            if (ui_button_ex(&ui, bx, by, bw, H_CTL, ti[i], tl[i], UI_GHOST)) {
                if (i == 0) { if (!modified) new_doc(); else say(true, "%s", "Save your changes first (Ctrl+S)"); }
                else if (i == 1) open_prompt(M_OPEN);
                else if (i == 2) do_save();
                else open_prompt(M_SAVEAS);
            }
            if (tips[i]) ui_tooltip(&ui, bx, by, bw, H_CTL, tips[i]);
            bx += bw + SP_2;
        }
        {
            const char *t = path[0] ? path : "Untitled";
            int maxw = W - bx - SP_4 - SP_5, nw = gfx_text_width(T_BODY, t);
            ui_text_fit(&ui, T_BODY, W - SP_4 - (nw < maxw ? nw : maxw), (tbH - T_BODY->height) / 2, maxw, t, UI_TEXT2);
        }

        /* editor */
        struct canvas cc = *c;
        canvas_clip(&cc, tx0 - SP_2, ey, ew - tx0 + SP_2, eh);
        struct canvas gc = *c;
        canvas_clip(&gc, ex, ey, gutter, eh);
        int lo, hi;
        sel_range(&lo, &hi);
        int curl = line_of(cur);
        for (int i = 0; i < vis + 1 && top + i < nlines; i++) {
            int l = top + i, y = ey + SP_2 + i * LH;
            if (y > ey + eh) break;
            int s = lines[l], e = line_end(l);
            if (l == curl && lo == hi) gfx_fill(&cc, tx0 - SP_2, y, ew, LH, ui_th.hover_ov);
            char num[16];
            snprintf(num, sizeof num, "%d", l + 1);
            gfx_text(&gc, mf, ex + gutter - SP_4 - gfx_text_width(mf, num), y + (LH - mf->height) / 2, num, l == curl ? UI_TEXT : UI_TEXT3);
            /* selection */
            if (hi > lo && hi > s && lo <= e) {
                int a = lo > s ? lo : s, b = hi < e ? hi : e;
                int x0 = tx0 + vcol(l, a) * CW - hx, x1 = tx0 + vcol(l, b) * CW - hx;
                if (hi > e) x1 += CW / 2;                       /* newline is selected too */
                gfx_fill(&cc, x0, y, x1 - x0, LH, UI_SELECTION);
            }
            int vc = 0;
            for (int j = s; j < e; j++) {
                if (buf[j] == '\t') { vc = (vc / TAB + 1) * TAB; continue; }
                int px = tx0 + vc * CW - hx;
                if (px > ew) break;
                if (px > tx0 - CW && buf[j] != ' ') gfx_char(&cc, mf, px, y + (LH - mf->height) / 2, buf[j], UI_TEXT);
                vc++;
            }
        }
        /* caret */
        uint32_t ci = now - last_act;
        if (!modal && ui.focused && (ci >= D_CARET_IDLE || (ci / D_CARET) % 2 == 0) && curl >= top && curl < top + vis + 1) {
            int cx = tx0 + vcol(curl, cur) * CW - hx, cy = ey + SP_2 + (curl - top) * LH;
            gfx_fill(&cc, cx - 1, cy + SP_1, SP_1, LH - 2 * SP_1, UI_TEXT);
        }
        ui_vscroll(&ui, W - 12, ey + SP_1, eh - 2 * SP_1, nlines, vis, &top);

        if (find_open && !modal) {                              /* popover at the top right of the editor, like Code */
            gfx_elev(c, fbx, fby, fbw, fbh, R_3, E2, ui_th.elev[E2]);
            gfx_round_fill(c, fbx, fby, fbw, fbh, R_3, ui_th.overlay);
            gfx_round_rect(c, fbx, fby, fbw, fbh, R_3, UI_LINE);
            if (ui_input(&ui, fq_x, fq_y, fq_w, fq_h, fq, sizeof fq, "Find", ICON_SEARCH, fq[0] && !fc_total ? UI_ERROR : 0)) find_next(false);
            find_stats();
            char cnt[32];
            if (!fq[0]) cnt[0] = 0;
            else if (!fc_total) strcpy(cnt, "No results");
            else if (fc_index) snprintf(cnt, sizeof cnt, "%d of %d", fc_index, fc_total);
            else snprintf(cnt, sizeof cnt, "%d found", fc_total);
            int bx = fbx + fbw - SP_3 - H_CTL;
            if (ui_icon_button(&ui, bx, fq_y, ICON_CLOSE, 0, "Close \u00b7 Esc")) { find_open = false; ui.focus = 0; }
            bx -= H_CTL + SP_2; if (ui_icon_button(&ui, bx, fq_y, ICON_CHEVRON_DOWN, fq[0] ? 0 : UI_DISABLED, "Next match \u00b7 F3")) find_next(false);
            bx -= H_CTL + SP_2; if (ui_icon_button(&ui, bx, fq_y, ICON_CHEVRON_UP, fq[0] ? 0 : UI_DISABLED, "Previous match \u00b7 Shift+F3")) find_next(true);
            bx -= H_CTL + SP_2; if (ui_icon_button(&ui, bx, fq_y, ICON_MATCH_CASE, find_case ? UI_ON : 0, "Match case")) find_case = !find_case;
            ui_text_fit(&ui, T_CAPTION, fq_x + fq_w + SP_3, fq_y + (fq_h - T_CAPTION->height) / 2, bx - SP_2 - (fq_x + fq_w + SP_3), cnt, fq[0] && !fc_total ? UI_BAD : UI_TEXT2);
            if (replace_open) {
                if (ui_input(&ui, fq_x, rq_y, fq_w, fq_h, rq, sizeof rq, "Replace", ICON_REPLACE, 0)) replace_one();
                int b1 = ui_button_w(UI_NOICON, "Replace"), b2 = ui_button_w(UI_NOICON, "Replace all");
                if (ui_button(&ui, fq_x + fq_w + SP_3, rq_y, b1, fq_h, "Replace", UI_SECONDARY)) replace_one();
                if (ui_button(&ui, fq_x + fq_w + SP_3 + b1 + SP_3, rq_y, b2, fq_h, "Replace all", UI_SECONDARY)) replace_all();
            }
        }

        /* status bar */
        char st[96], rt[64];
        int sn = snprintf(st, sizeof st, "Ln %d, Col %d", curl + 1, vcol(curl, cur) + 1);
        if (hi > lo) snprintf(st + sn, sizeof st - sn, "  \u00b7  %d selected", hi - lo);
        snprintf(rt, sizeof rt, "%d characters, %d line%s", len, nlines, nlines == 1 ? "" : "s");
        ui_status(&ui, 0, H - sbH, W, st, rt);

        /* dialogs */
        if (mode == M_OPEN || mode == M_SAVEAS) {
            int r = ui_prompt(&ui, mode == M_OPEN ? "Open file" : "Save as", mode == M_OPEN ? "Open" : "Save", prompt, sizeof prompt);
            if (r == 1 && prompt[0]) {
                bool ok = mode == M_OPEN ? load(prompt) : save(prompt);
                if (ok) { mode = M_EDIT; if (closing) break; }
            } else if (r == -1) { mode = M_EDIT; closing = false; }
        } else if (mode == M_GOTO) {
            int r = ui_prompt(&ui, "Go to line", "Go", prompt, 12);
            if (r != 0) {
                mode = M_EDIT;
                if (r == 1) {
                    int ln = atoi(prompt);
                    ln = ln < 1 ? 1 : ln > nlines ? nlines : ln;
                    move_to(lines[ln - 1], false, false);
                    top = ln - 1 - vis / 2;
                }
            }
        } else if (mode == M_CLOSE) {
            char q[PATH_MAX + 40];
            snprintf(q, sizeof q, "Save changes to \u201c%s\u201d?", path[0] ? base(path) : "Untitled");
            int r = ui_dialog_ex(&ui, q, "Your changes will be lost if you don't save them.", "Don't save", "Cancel", "Save", UI_PRIMARY);
            if (r == 2) break;
            if (r == -1) mode = M_EDIT;
            if (r == 1) {
                if (path[0]) { if (save(path)) break; mode = M_EDIT; }
                else { closing = true; open_prompt(M_SAVEAS); }
            }
        }
        update_title(w);
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
