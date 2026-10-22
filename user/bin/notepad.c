/* notepad - plain text editor: caret, selection, clipboard, line numbers, open/save */
#include <ui.h>
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
static char msg[96];
static uint32_t msg_until;
static bool msg_bad;

enum { M_EDIT, M_OPEN, M_SAVEAS, M_CLOSE };
static int mode = M_EDIT;
static bool closing;               /* Save pressed in the close dialog: exit once saved */
static char prompt[PATH_MAX];

static int CW, LH;

static void say(bool bad, const char *fmt, const char *arg) {
    snprintf(msg, sizeof msg, fmt, arg);
    msg_bad = bad;
    msg_until = uptime_ms() + 4000;
}

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

static void del_range(int lo, int hi) {
    if (hi <= lo) return;
    memmove(buf + lo, buf + hi, len - hi);
    len -= hi - lo;
    buf[len] = 0;
    cur = anchor = lo;
    modified = lines_dirty = true;
}

static void insert(const char *s, int n) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (len - (hi - lo) + n > MAX_FILE) { say(true, "%s", "File too large"); return; }
    del_range(lo, hi);
    ensure(n);
    memmove(buf + cur + n, buf + cur, len - cur);
    memcpy(buf + cur, s, n);
    len += n;
    buf[len] = 0;
    cur = anchor = cur + n;
    modified = lines_dirty = true;
}

static void copy_sel(void) {
    int lo, hi;
    sel_range(&lo, &hi);
    if (hi > lo) gui_clip_set(buf + lo, hi - lo);
}

static void new_doc(void) {
    len = cur = anchor = 0;
    buf[0] = 0;
    modified = false;
    lines_dirty = true;
    path[0] = 0;
    top = hx = 0;
}

static bool load(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { say(true, "Cannot open %s", p); return false; }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); say(true, "%s is a folder", p); return false; }
    len = 0;
    for (;;) {
        ensure(4096);
        int n = read(fd, buf + len, 4096);
        if (n <= 0) break;
        len += n;
        if (len >= MAX_FILE) { say(true, "%s", "File truncated at 1 MiB"); break; }
    }
    close(fd);
    for (int i = 0; i < len; i++) if (buf[i] == '\r') { memmove(buf + i, buf + i + 1, len - i - 1); len--; i--; }
    buf[len] = 0;
    cur = anchor = 0;
    top = hx = 0;
    modified = false;
    lines_dirty = true;
    snprintf(path, sizeof path, "%s", p);
    return true;
}

static bool save(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { say(true, "Cannot write %s", p); return false; }
    int off = 0;
    while (off < len) {
        int n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); say(true, "Write failed: %s", p); return false; }
        off += n;
    }
    close(fd);
    snprintf(path, sizeof path, "%s", p);
    modified = false;
    say(false, "Saved %s", p);
    return true;
}

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

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
}

static void handle_key(const struct gui_event *k, int vis_lines) {
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    int l = line_of(cur);
    if (ctrl) {
        int key = k->key < 256 ? tolower(k->key) : k->key;
        switch (key) {
        case 's': do_save(); break;
        case 'o': open_prompt(M_OPEN); break;
        case 'n': if (!modified) new_doc(); else say(true, "%s", "Unsaved changes - save first (Ctrl+S)"); break;
        case 'a': anchor = 0; cur = len; break;
        case 'c': copy_sel(); break;
        case 'x': copy_sel(); { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); } break;
        case 'v': { int n; char *c = gui_clip_get(&n); if (c) insert(c, n); free(c); break; }
        case KEY_LEFT: { int i = cur; while (i > 0 && !isword(buf[i - 1])) i--; while (i > 0 && isword(buf[i - 1])) i--; move_to(i, shift, false); break; }
        case KEY_RIGHT: { int i = cur; while (i < len && !isword(buf[i])) i++; while (i < len && isword(buf[i])) i++; move_to(i, shift, false); break; }
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
        if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); }
        else if (cur > 0) del_range(cur - 1, cur);
        want_col = -1; last_act = uptime_ms();
        return;
    case KEY_DELETE:
        if (cur != anchor) { int lo, hi; sel_range(&lo, &hi); del_range(lo, hi); }
        else if (cur < len) del_range(cur, cur + 1);
        want_col = -1; last_act = uptime_ms();
        return;
    case KEY_ENTER: {
        char ins[128] = "\n";                         /* auto-indent */
        int n = 1, s = lines[line_of(cur < anchor ? cur : anchor)];
        while (n < 120 && (buf[s + n - 1] == ' ' || buf[s + n - 1] == '\t') && s + n - 1 < cur) { ins[n] = buf[s + n - 1]; n++; }
        insert(ins, n);
        want_col = -1; last_act = uptime_ms();
        return;
    }
    case KEY_TAB: insert("    ", TAB); want_col = -1; last_act = uptime_ms(); return;
    case KEY_ESC: anchor = cur; return;
    }
    if (k->ch >= 32 && k->ch < 127 && !(k->mods & MOD_ALT)) {
        char ch = k->ch;
        insert(&ch, 1);
        want_col = -1;
        last_act = uptime_ms();
    }
}

static void update_title(struct gui_window *w) {
    static char last[96];
    char t[96];
    snprintf(t, sizeof t, "%s%s - Notepad", modified ? "* " : "", path[0] ? base(path) : "Untitled");
    if (strcmp(t, last)) { strcpy(last, t); gui_set_title(w, t); }
}

int main(int argc, char **argv) {
    cap = 4096;
    buf = malloc(cap);
    buf[0] = 0;
    struct gui_window *w = gui_window_create("Untitled - Notepad", 760, 520, 0);
    if (!w) { perror("notepad"); return 1; }
    CW = font_mono.g['M' - 32].adv;
    LH = font_mono.height + 5;
    struct ui ui;
    ui_init(&ui, w);
    if (argc > 1) load(argv[1]);
    last_act = uptime_ms();
    bool dragging = false;
    for (;;) {
        if (!ui_poll(&ui, 250)) {
            if (!modified || mode == M_CLOSE) break;
            ui.closed = false;                          /* ask before throwing edits away */
            mode = M_CLOSE;
        }
        uint32_t now = uptime_ms();
        int W = w->w, H = w->h;
        bool modal = mode != M_EDIT;
        int tbH = 44, sbH = 28;
        int gutter = 0;
        reindex();
        for (int n = nlines; n; n /= 10) gutter += CW;
        gutter += 28;
        int ex = 0, ey = tbH, ew = W, eh = H - tbH - sbH;      /* editor area */
        int tx0 = ex + gutter + 6;                             /* text origin */
        int vis = eh / LH;

        /* hide input from the background while a dialog is up */
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, sd = ui.dbl;
        int nk = ui.nkeys;
        struct gui_event saved_keys[32];
        memcpy(saved_keys, ui.keys, sizeof saved_keys);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.dbl = false; ui.nkeys = 0; }

        /* keys */
        struct gui_event k;
        while (ui_key(&ui, &k)) {
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
        bool in_ed = ui_hover(&ui, tx0, ey, ew - tx0, eh);
        if (in_ed && ui.scroll) top += ui.scroll * 3;
        if (ui.pressed && in_ed) {
            int l = top + (ui.my - ey - 4) / LH;
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
            int l = top + (ui.my - ey - 4) / LH;
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
        gfx_fill(c, 0, 0, W, tbH, UI_PANEL);
        gfx_fill(c, 0, tbH - 1, W, 1, UI_BORDER);
        int bx = 8;
        const char *tl[] = { "New", "Open", "Save", "Save As" };
        for (int i = 0; i < 4; i++) {
            int bw = gfx_text_width(&font_ui, tl[i]) + 24;
            if (ui_tool(&ui, bx, 6, bw, 32, tl[i], false)) {
                if (i == 0) { if (!modified) new_doc(); else say(true, "%s", "Unsaved changes - save first"); }
                else if (i == 1) open_prompt(M_OPEN);
                else if (i == 2) do_save();
                else open_prompt(M_SAVEAS);
            }
            bx += bw + 2;
        }
        {
            char t[PATH_MAX + 4];
            snprintf(t, sizeof t, "%s%s", path[0] ? path : "Untitled", "");
            int nw = gfx_text_width(&font_ui, t), maxw = W - bx - 24;
            int x = W - 14 - (nw < maxw ? nw : maxw);
            if (modified) gfx_circle_fill(c, x - 12, tbH / 2, 4, UI_ACCENT2);
            ui_text_fit(&ui, &font_ui, x, (tbH - font_ui.height) / 2, maxw, t, UI_TEXT_DIM);
        }

        /* editor */
        gfx_fill(c, ex, ey, ew, eh, RGB(0x12, 0x11, 0x1C));
        gfx_fill(c, ex, ey, gutter, eh, RGB(0x19, 0x18, 0x26));
        gfx_fill(c, ex + gutter, ey, 1, eh, UI_BORDER);
        struct canvas cc = *c;
        canvas_clip(&cc, tx0, ey, ew - tx0 - 10, eh);
        struct canvas gc = *c;
        canvas_clip(&gc, ex, ey, gutter, eh);
        int lo, hi;
        sel_range(&lo, &hi);
        int curl = line_of(cur);
        for (int i = 0; i < vis + 1 && top + i < nlines; i++) {
            int l = top + i, y = ey + 4 + i * LH;
            if (y > ey + eh) break;
            int s = lines[l], e = line_end(l);
            if (l == curl && lo == hi) gfx_fill(&cc, tx0 - 4, y - 1, ew, LH, RGBA(0xB3, 0x88, 0xFF, 14));
            char num[16];
            snprintf(num, sizeof num, "%d", l + 1);
            gfx_text(&gc, &font_mono, ex + gutter - 12 - gfx_text_width(&font_mono, num), y + 1, num, l == curl ? UI_ACCENT : RGB(0x5E, 0x5A, 0x7C));
            /* selection */
            if (hi > lo && hi > s && lo <= e) {
                int a = lo > s ? lo : s, b = hi < e ? hi : e;
                int x0 = tx0 + vcol(l, a) * CW - hx, x1 = tx0 + vcol(l, b) * CW - hx;
                if (hi > e) x1 += CW / 2;                       /* newline is selected too */
                gfx_fill(&cc, x0, y - 1, x1 - x0, LH, RGBA(0xB3, 0x88, 0xFF, 100));
            }
            int vc = 0;
            for (int j = s; j < e; j++) {
                if (buf[j] == '\t') { vc = (vc / TAB + 1) * TAB; continue; }
                int px = tx0 + vc * CW - hx;
                if (px > ew) break;
                if (px > tx0 - CW && buf[j] != ' ') gfx_char(&cc, &font_mono, px, y + 1, buf[j], UI_TEXT);
                vc++;
            }
        }
        /* caret */
        if (!modal && ui.focused && ((now - last_act) / 500) % 2 == 0 && curl >= top && curl < top + vis + 1) {
            int cx = tx0 + vcol(curl, cur) * CW - hx, cy = ey + 4 + (curl - top) * LH;
            gfx_fill(&cc, cx, cy, 2, LH - 2, UI_ACCENT);
        }
        ui_vscroll(&ui, W - 12, ey + 2, eh - 4, nlines, vis, &top);

        /* status bar */
        gfx_fill(c, 0, H - sbH, W, sbH, UI_PANEL);
        gfx_fill(c, 0, H - sbH, W, 1, UI_BORDER);
        char st[96];
        snprintf(st, sizeof st, "Ln %d, Col %d", curl + 1, vcol(curl, cur) + 1);
        gfx_text(c, &font_ui, 14, H - sbH + 6, st, UI_TEXT_DIM);
        if (hi > lo) {
            snprintf(st, sizeof st, "%d selected", hi - lo);
            gfx_text(c, &font_ui, 140, H - sbH + 6, st, UI_ACCENT);
        }
        snprintf(st, sizeof st, "%d chars  %d lines", len, nlines);
        int sw = gfx_text_width(&font_ui, st);
        gfx_text(c, &font_ui, W - 14 - sw, H - sbH + 6, st, UI_TEXT_DIM);
        if (msg[0] && now < msg_until) {
            int mw = gfx_text_width(&font_ui, msg);
            gfx_text(c, &font_ui, (W - mw) / 2, H - sbH + 6, msg, msg_bad ? UI_BAD : UI_GOOD);
        }

        /* dialogs */
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.dbl = sd;
            ui.nkeys = nk;
            memcpy(ui.keys, saved_keys, sizeof saved_keys);
            if (mode == M_OPEN || mode == M_SAVEAS) {
                int r = ui_prompt(&ui, mode == M_OPEN ? "Open file" : "Save as", mode == M_OPEN ? "Open" : "Save", prompt, sizeof prompt);
                if (r == 1 && prompt[0]) {
                    bool ok = mode == M_OPEN ? load(prompt) : save(prompt);
                    if (ok) { mode = M_EDIT; if (closing) break; }
                } else if (r == -1) mode = M_EDIT;
            } else {                                              /* M_CLOSE */
                gfx_fill(c, 0, 0, W, H, RGBA(0, 0, 0, 150));
                int pw = 400, ph = 130, px = (W - pw) / 2, py = (H - ph) / 2;
                gfx_shadow(c, px, py, pw, ph, 12, 14, 120);
                gfx_round_fill(c, px, py, pw, ph, 12, UI_PANEL);
                gfx_round_rect(c, px, py, pw, ph, 12, RGBA(255, 255, 255, 40));
                gfx_text(c, &font_ui_bold, px + 20, py + 16, "Unsaved changes", UI_TEXT);
                gfx_text(c, &font_ui, px + 20, py + 42, "Save before closing?", UI_TEXT_DIM);
                if (ui_button(&ui, px + pw - 20 - 100, py + ph - 50, 100, 34, "Save", UI_ACCENT_BTN)) {
                    if (path[0]) { if (save(path)) break; mode = M_EDIT; }
                    else { closing = true; open_prompt(M_SAVEAS); }
                }
                if (ui_button(&ui, px + pw - 20 - 220, py + ph - 50, 110, 34, "Discard", UI_NORMAL_BTN)) break;
                if (ui_button(&ui, px + 20, py + ph - 50, 90, 34, "Cancel", UI_GHOST_BTN)) mode = M_EDIT;
            }
        }
        update_title(w);
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
