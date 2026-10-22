/* files - file manager */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syscall.h>
#include "image.h"

#define MAX_ENT 1024
#define HIST_MAX 16
#define SIDE_W 168
#define ROW_H 30

struct ent {
    char name[96];
    bool dir, exe;
    uint32_t size, mtime;
};

static struct ent *ents;
static int nents;
static char cwd[PATH_MAX] = "/home";
static char hist[HIST_MAX][PATH_MAX];
static int nhist;
static char pathbox[PATH_MAX];
static int sel = -1, scroll;
static int tz_min;
static char msg[128];
static uint32_t msg_until;
static bool msg_bad;

enum { M_NONE, M_MENU, M_NEWDIR, M_RENAME, M_DELETE };
static int mode;
static char namebuf[96];
static int menu_x, menu_y, menu_row;       /* menu_row = entry under the pointer, -1 for background */

static void say(bool bad, const char *fmt, const char *arg) {
    snprintf(msg, sizeof msg, fmt, arg);
    msg_bad = bad;
    msg_until = uptime_ms() + 4000;
}

static void join(char *out, const char *dir, const char *name) {
    snprintf(out, PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static int cmp_ent(const void *a, const void *b) {
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void refresh(const char *keep) {
    DIR *d = opendir(cwd);
    nents = 0;
    if (!ents) ents = malloc(MAX_ENT * sizeof *ents);
    if (!d) { say(true, "Cannot open %s", cwd); return; }
    struct dirent *de;
    while ((de = readdir(d)) && nents < MAX_ENT) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct ent *e = &ents[nents++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char p[PATH_MAX];
        join(p, cwd, de->d_name);
        struct stat st;
        memset(&st, 0, sizeof st);
        e->dir = de->d_type == DT_DIR;
        if (stat(p, &st) == 0) {
            e->dir = S_ISDIR(st.st_mode);
            e->exe = !e->dir && S_ISREG(st.st_mode) && (st.st_mode & 0111);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        }
    }
    closedir(d);
    qsort(ents, nents, sizeof *ents, cmp_ent);
    sel = -1;
    if (keep) for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, keep)) sel = i;
    snprintf(pathbox, sizeof pathbox, "%s", cwd);
}

static void go(const char *p, bool push) {
    char np[PATH_MAX];
    if (!realpath(p, np)) { say(true, "No such folder: %s", p); snprintf(pathbox, sizeof pathbox, "%s", cwd); return; }
    struct stat st;
    if (stat(np, &st) || !S_ISDIR(st.st_mode)) { say(true, "Not a folder: %s", p); snprintf(pathbox, sizeof pathbox, "%s", cwd); return; }
    if (push && strcmp(np, cwd)) {
        if (nhist == HIST_MAX) { memmove(hist[0], hist[1], (HIST_MAX - 1) * PATH_MAX); nhist--; }
        snprintf(hist[nhist++], PATH_MAX, "%s", cwd);
    }
    snprintf(cwd, sizeof cwd, "%s", np);
    scroll = 0;
    refresh(NULL);
}

static void go_up(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", cwd);
    char *s = strrchr(p, '/');
    char keep[96] = "";
    if (s) snprintf(keep, sizeof keep, "%s", s + 1);
    if (s == p) p[1] = 0; else if (s) *s = 0;
    if (!strcmp(p, cwd)) return;
    go(p, true);
    for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, keep)) sel = i;
}

static void go_back(void) {
    if (!nhist) return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", hist[--nhist]);
    go(p, false);
}

static void open_entry(int i) {
    if (i < 0 || i >= nents) return;
    struct ent *e = &ents[i];
    char p[PATH_MAX];
    join(p, cwd, e->name);
    if (e->dir) { go(p, true); return; }
    char *argv[3];
    int pid;
    if (e->exe) {
        argv[0] = e->name; argv[1] = NULL;
        pid = spawnv(p, argv);
    } else if (image_is_ext(e->name)) {
        argv[0] = "viewer"; argv[1] = p; argv[2] = NULL;
        pid = spawnv("/bin/viewer", argv);
    } else {
        argv[0] = "notepad"; argv[1] = p; argv[2] = NULL;
        pid = spawnv("/bin/notepad", argv);
    }
    if (pid < 0) say(true, "Could not start %s", e->name);
    else say(false, e->exe ? "Started %s" : "Opening %s", e->name);
}

/* ---- file operations ---- */
static int rm_rf(const char *p, int depth) {
    struct stat st;
    if (stat(p, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    if (depth > 8) return -1;
    DIR *d = opendir(p);
    if (!d) return -1;
    char (*names)[96] = malloc(MAX_ENT * 96);
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_ENT) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(names[n++], 96, "%s", de->d_name);
    }
    closedir(d);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char c[PATH_MAX];
        join(c, p, names[i]);
        if (rm_rf(c, depth + 1)) rc = -1;
    }
    free(names);
    return rc ? rc : rmdir(p);
}

static bool protected_path(const char *p) {                    /* "/" and the top-level system folders */
    const char *s = p + 1;
    return !p[1] || !strchr(s, '/');
}

static void do_delete(void) {
    if (sel < 0) return;
    char p[PATH_MAX];
    join(p, cwd, ents[sel].name);
    if (protected_path(p)) { say(true, "%s is protected", ents[sel].name); return; }
    if (rm_rf(p, 0)) say(true, "Could not delete %s", ents[sel].name);
    else say(false, "Deleted %s", ents[sel].name);
    refresh(NULL);
}

static void do_mkdir(const char *name) {
    if (!name[0] || strchr(name, '/')) { say(true, "%s", "Invalid name"); return; }
    char p[PATH_MAX];
    join(p, cwd, name);
    if (mkdir(p, 0755)) { say(true, "Could not create %s", name); return; }
    say(false, "Created %s", name);
    refresh(name);
}

static void do_rename(const char *name) {
    if (sel < 0) return;
    if (!name[0] || strchr(name, '/')) { say(true, "%s", "Invalid name"); return; }
    char a[PATH_MAX], b[PATH_MAX];
    join(a, cwd, ents[sel].name);
    join(b, cwd, name);
    if (rename(a, b)) { say(true, "Could not rename to %s", name); return; }
    say(false, "Renamed to %s", name);
    refresh(name);
}

/* ---- drawing ---- */
static void icon_folder(struct canvas *c, int x, int y) {
    color_t body = RGB(0xFF, 0xC2, 0x4B), back = RGB(0xE0, 0x9A, 0x2B);
    gfx_round_fill(c, x, y + 2, 8, 6, 2, back);
    gfx_round_fill(c, x, y + 4, 20, 14, 3, back);
    gfx_round_fill(c, x, y + 7, 20, 11, 3, body);
}
static void icon_file(struct canvas *c, int x, int y) {
    gfx_round_fill(c, x + 2, y, 15, 19, 3, RGB(0xD9, 0xD5, 0xF0));
    gfx_fill(c, x + 11, y, 6, 6, RGB(0x8E, 0x88, 0xB5));
    for (int i = 0; i < 4; i++) gfx_fill(c, x + 5, y + 8 + i * 3, 9 - (i == 3) * 4, 1, RGB(0x7A, 0x74, 0xA0));
}
static void icon_exe(struct canvas *c, int x, int y) {
    gfx_round_fill(c, x + 1, y + 1, 18, 18, 5, RGB(0x7C, 0x4D, 0xFF));
    gfx_round_fill(c, x + 1, y + 1, 18, 9, 5, RGB(0x95, 0x6C, 0xFF));
    for (int i = 0; i < 9; i++) gfx_fill(c, x + 7, y + 5 + i, 7 - (i < 5 ? 4 - i : i - 4) , 1, RGB(255, 255, 255));
    gfx_fill(c, x + 6, y + 5, 2, 9, RGB(255, 255, 255));
}
static void icon_home(struct canvas *c, int x, int y, color_t col) {
    gfx_line_aa(c, x + 2, y + 9, x + 10, y + 2, 2.2f, col);
    gfx_line_aa(c, x + 10, y + 2, x + 18, y + 9, 2.2f, col);
    gfx_fill(c, x + 5, y + 9, 10, 8, col);
    gfx_fill(c, x + 9, y + 12, 3, 5, UI_PANEL);
}
static void icon_drive(struct canvas *c, int x, int y, color_t col) {
    gfx_round_fill(c, x + 1, y + 5, 18, 11, 3, col);
    gfx_circle_fill(c, x + 15, y + 11, 1, UI_PANEL);
}
static void icon_gear(struct canvas *c, int x, int y, color_t col) {
    gfx_circle_fill(c, x + 10, y + 10, 8, col);
    gfx_circle_fill(c, x + 10, y + 10, 3, UI_PANEL);
}

static void fmt_size(char *o, int n, uint32_t s) {
    if (s < 1024) snprintf(o, n, "%u B", s);
    else if (s < 1024 * 1024) snprintf(o, n, "%u.%u KiB", s / 1024, s % 1024 * 10 / 1024);
    else snprintf(o, n, "%u.%u MiB", s >> 20, (s & 0xFFFFF) * 10 >> 20);
}

static void fmt_time(char *o, int n, uint32_t t) {
    if (!t) { snprintf(o, n, "--"); return; }
    time_t lt = (time_t)(t + tz_min * 60);
    struct tm tm;
    gmtime_r(&lt, &tm);
    snprintf(o, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Files", 880, 580, 0);
    if (!w) { perror("files"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    FILE *f = fopen("/etc/tz", "r");
    if (f) { char b[16] = { 0 }; if (fgets(b, sizeof b, f)) tz_min = atoi(b); fclose(f); }
    go(argc > 1 ? argv[1] : "/home", false);
    if (!nents && !strcmp(cwd, "/home")) { /* keep going even if /home is empty */ }
    do {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}              /* reap apps we launched */
        uint32_t now = uptime_ms();
        int W = w->w, H = w->h;
        bool modal = mode != M_NONE;
        int tbH = 52, sbH = 28;

        /* hide input from the background while a dialog or menu is up */
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, sd = ui.dbl, srp = ui.rpressed;
        int nk = ui.nkeys, ssc = ui.scroll;
        struct gui_event saved[32];
        memcpy(saved, ui.keys, sizeof saved);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.dbl = ui.rpressed = false; ui.nkeys = 0; ui.scroll = 0; }

        /* ---- layout ---- */
        int lx = SIDE_W, ly = tbH, lw = W - SIDE_W, lh = H - tbH - sbH;
        int hdrH = 28, ry0 = ly + hdrH + 4;
        int listH = ly + lh - ry0 - 4;
        int vis = listH / ROW_H;
        int modx = W - 170, sizex = modx - 100;

        /* ---- list interaction (before drawing, so the frame reflects it) ---- */
        bool in_list = ui_hover(&ui, lx, ry0, lw, listH);
        if (in_list && ui.scroll) scroll += ui.scroll * 2;
        struct gui_event k;
        bool pathfocus = ui.focus == ui_id(94, 8, W - 94 - 260, 36);
        while (!pathfocus && ui_key(&ui, &k)) {
            if (k.key == KEY_DOWN && sel < nents - 1) sel++;
            else if (k.key == KEY_UP && sel > 0) sel--;
            else if (k.key == KEY_UP && sel < 0 && nents) sel = 0;
            else if (k.key == KEY_PGDN) sel = sel + vis < nents ? sel + vis : nents - 1;
            else if (k.key == KEY_PGUP) sel = sel > vis ? sel - vis : 0;
            else if (k.key == KEY_HOME && nents) sel = 0;
            else if (k.key == KEY_END && nents) sel = nents - 1;
            else if (k.key == KEY_ENTER) open_entry(sel);
            else if (k.key == KEY_BACKSPACE) go_up();
            else if (k.key == KEY_LEFT && (k.mods & MOD_ALT)) go_back();
            else if (k.key == KEY_UP && (k.mods & MOD_ALT)) go_up();
            else if (k.key == KEY_DELETE && sel >= 0) mode = M_DELETE;
            else if (k.key == KEY_F2 && sel >= 0) { snprintf(namebuf, sizeof namebuf, "%s", ents[sel].name); mode = M_RENAME; ui.focus = 0; }
            else if (k.key == KEY_F5) refresh(sel >= 0 ? ents[sel].name : NULL);
            else if (k.key == 'n' && (k.mods & MOD_CTRL)) { namebuf[0] = 0; mode = M_NEWDIR; ui.focus = 0; }
            if (sel >= 0 && sel < scroll) scroll = sel;
            if (sel >= scroll + vis) scroll = sel - vis + 1;
        }
        if (scroll > nents - vis) scroll = nents - vis;
        if (scroll < 0) scroll = 0;
        if (ui.pressed && in_list) {
            int i = scroll + (ui.my - ry0) / ROW_H;
            if (ui.mx < lx + lw - 14) {
                if (i < nents) { sel = i; if (ui.dbl) open_entry(i); }
                else sel = -1;
            }
        }
        if (ui.rpressed && ui_hover(&ui, lx, ly, lw, lh)) {
            int i = ui.my >= ry0 && ui.my < ry0 + listH ? scroll + (ui.my - ry0) / ROW_H : -1;
            if (i >= nents) i = -1;
            if (i >= 0) sel = i;
            menu_row = i;
            menu_x = ui.mx; menu_y = ui.my;
            mode = M_MENU;
        }

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        /* toolbar */
        gfx_fill(c, 0, 0, W, tbH, UI_PANEL);
        gfx_fill(c, 0, tbH - 1, W, 1, UI_BORDER);
        if (ui_tool(&ui, 10, 8, 36, 36, "", false)) go_back();
        gfx_line_aa(c, 36, 26, 20, 26, 2.2f, nhist ? UI_TEXT : UI_TEXT_DIM);
        gfx_line_aa(c, 20, 26, 26, 20, 2.2f, nhist ? UI_TEXT : UI_TEXT_DIM);
        gfx_line_aa(c, 20, 26, 26, 32, 2.2f, nhist ? UI_TEXT : UI_TEXT_DIM);
        if (ui_tool(&ui, 50, 8, 36, 36, "", false)) go_up();
        gfx_line_aa(c, 68, 34, 68, 18, 2.2f, UI_TEXT);
        gfx_line_aa(c, 68, 18, 62, 24, 2.2f, UI_TEXT);
        gfx_line_aa(c, 68, 18, 74, 24, 2.2f, UI_TEXT);
        if (ui_textbox(&ui, 94, 8, W - 94 - 260, 36, pathbox, sizeof pathbox, "path")) { go(pathbox, true); ui.focus = 0; }
        int bx = W - 256;
        if (ui_tool(&ui, bx, 8, 92, 36, "New Folder", false)) { namebuf[0] = 0; mode = M_NEWDIR; ui.focus = 0; }
        if (ui_tool(&ui, bx + 94, 8, 76, 36, "Rename", false) && sel >= 0) { snprintf(namebuf, sizeof namebuf, "%s", ents[sel].name); mode = M_RENAME; ui.focus = 0; }
        if (ui_tool(&ui, bx + 172, 8, 76, 36, "Delete", false) && sel >= 0) mode = M_DELETE;

        /* sidebar */
        gfx_fill(c, 0, tbH, SIDE_W, H - tbH - sbH, RGB(0x1A, 0x19, 0x28));
        gfx_fill(c, SIDE_W - 1, tbH, 1, H - tbH - sbH, UI_BORDER);
        gfx_text(c, &font_ui_bold, 18, tbH + 14, "PLACES", UI_TEXT_DIM);
        static const struct { const char *label, *path; int ico; } places[] = {
            { "Home", "/home", 0 }, { "Root", "/", 1 }, { "Programs", "/bin", 2 },
            { "Config", "/etc", 2 }, { "Processes", "/proc", 3 }, { "Devices", "/dev", 3 },
        };
        for (int i = 0; i < 6; i++) {
            int y = tbH + 40 + i * 38;
            bool act = !strcmp(cwd, places[i].path), hov = ui_hover(&ui, 8, y, SIDE_W - 16, 34);
            if (act) gfx_round_fill(c, 8, y, SIDE_W - 16, 34, 8, RGBA(0xB3, 0x88, 0xFF, 80));
            else if (hov) gfx_round_fill(c, 8, y, SIDE_W - 16, 34, 8, RGBA(255, 255, 255, 18));
            color_t ic = act ? UI_ACCENT : UI_TEXT_DIM;
            switch (places[i].ico) {
            case 0: icon_home(c, 18, y + 7, ic); break;
            case 1: icon_drive(c, 18, y + 7, ic); break;
            case 2: gfx_round_fill(c, 19, y + 11, 18 - 0, 13, 3, ic); gfx_round_fill(c, 19, y + 8, 8, 5, 2, ic); break;
            default: icon_gear(c, 18, y + 7, ic); break;
            }
            gfx_text(c, &font_ui, 46, y + 8, places[i].label, act ? UI_TEXT : UI_TEXT);
            if (ui.pressed && hov) go(places[i].path, true);
        }

        /* column headers */
        gfx_text(c, &font_ui_bold, lx + 54, ly + 9, "Name", UI_TEXT_DIM);
        gfx_text(c, &font_ui_bold, sizex, ly + 9, "Size", UI_TEXT_DIM);
        gfx_text(c, &font_ui_bold, modx, ly + 9, "Modified", UI_TEXT_DIM);
        gfx_fill(c, lx + 8, ly + hdrH, lw - 16, 1, UI_BORDER);

        struct canvas cc = *c;
        canvas_clip(&cc, lx, ry0, lw, listH);
        if (!nents) gfx_text_center(&cc, &font_ui, lx, ry0 + 40, lw, "This folder is empty", UI_TEXT_DIM);
        for (int i = scroll; i < nents && i < scroll + vis + 1; i++) {
            int y = ry0 + (i - scroll) * ROW_H;
            struct ent *e = &ents[i];
            bool hov = ui_hover(&ui, lx, y, lw - 14, ROW_H) && y + ROW_H <= ry0 + listH + ROW_H / 2;
            if (i == sel) gfx_round_fill(&cc, lx + 6, y + 1, lw - 24, ROW_H - 2, 7, RGBA(0xB3, 0x88, 0xFF, 85));
            else if (hov) gfx_round_fill(&cc, lx + 6, y + 1, lw - 24, ROW_H - 2, 7, RGBA(255, 255, 255, 16));
            int iy = y + (ROW_H - 20) / 2;
            if (e->dir) icon_folder(&cc, lx + 16, iy);
            else if (e->exe) icon_exe(&cc, lx + 16, iy);
            else icon_file(&cc, lx + 16, iy);
            int ty = y + (ROW_H - font_ui.height) / 2;
            struct canvas nc = cc;
            canvas_clip(&nc, lx, y, sizex - lx - 8, ROW_H);
            gfx_text(&nc, &font_ui, lx + 54, ty, e->name, UI_TEXT);
            char b[40];
            if (e->dir) snprintf(b, sizeof b, "--"); else fmt_size(b, sizeof b, e->size);
            gfx_text(&cc, &font_ui, sizex, ty, b, UI_TEXT_DIM);
            fmt_time(b, sizeof b, e->mtime);
            gfx_text(&cc, &font_ui, modx, ty, b, UI_TEXT_DIM);
        }
        ui_vscroll(&ui, W - 12, ry0, listH, nents, vis, &scroll);

        /* status bar */
        gfx_fill(c, 0, H - sbH, W, sbH, UI_PANEL);
        gfx_fill(c, 0, H - sbH, W, 1, UI_BORDER);
        char st[160];
        if (sel >= 0) {
            char sz[32];
            fmt_size(sz, sizeof sz, ents[sel].size);
            snprintf(st, sizeof st, "%d items   |   %s%s%s", nents, ents[sel].name, ents[sel].dir ? "" : "   ", ents[sel].dir ? "" : sz);
        } else snprintf(st, sizeof st, "%d items", nents);
        ui_text_fit(&ui, &font_ui, 14, H - sbH + 6, W / 2, st, UI_TEXT_DIM);
        if (msg[0] && now < msg_until) {
            int mw = gfx_text_width(&font_ui, msg);
            gfx_text(c, &font_ui, W - 14 - mw, H - sbH + 6, msg, msg_bad ? UI_BAD : UI_GOOD);
        }

        /* ---- dialogs and menu ---- */
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.dbl = sd; ui.rpressed = srp;
            ui.nkeys = nk; ui.scroll = ssc;
            memcpy(ui.keys, saved, sizeof saved);
        }
        if (mode != M_NONE) {
            bool fresh = !modal;                      /* opened this frame: the opening click must not close it */
            if (mode == M_MENU) {
                const char *items[5]; int ids[5], n = 0;
                if (menu_row >= 0) { items[n] = "Open"; ids[n++] = 0; items[n] = "Rename"; ids[n++] = 1; items[n] = "Delete"; ids[n++] = 2; }
                items[n] = "New Folder"; ids[n++] = 3;
                items[n] = "Refresh"; ids[n++] = 4;
                int mw = 160, mh = n * 32 + 12;
                int mx = menu_x + mw > W ? W - mw - 4 : menu_x, my = menu_y + mh > H ? H - mh - 4 : menu_y;
                gfx_shadow(c, mx, my, mw, mh, 10, 10, 120);
                gfx_round_fill(c, mx, my, mw, mh, 10, RGB(0x2A, 0x27, 0x3D));
                gfx_round_rect(c, mx, my, mw, mh, 10, RGBA(255, 255, 255, 45));
                int chosen = -1;
                for (int i = 0; i < n; i++) {
                    int y = my + 6 + i * 32;
                    bool hov = ui_hover(&ui, mx + 6, y, mw - 12, 32);
                    if (hov) gfx_round_fill(c, mx + 6, y, mw - 12, 32, 6, RGBA(0xB3, 0x88, 0xFF, 90));
                    gfx_text(c, &font_ui, mx + 18, y + 8, items[i], ids[i] == 2 ? UI_BAD : UI_TEXT);
                    if (ui.pressed && hov) chosen = ids[i];
                }
                if (chosen >= 0 || (!fresh && (ui.pressed || ui.rpressed))) mode = M_NONE;
                switch (chosen) {
                case 0: open_entry(menu_row); break;
                case 1: snprintf(namebuf, sizeof namebuf, "%s", ents[sel].name); mode = M_RENAME; break;
                case 2: mode = M_DELETE; break;
                case 3: namebuf[0] = 0; mode = M_NEWDIR; break;
                case 4: refresh(sel >= 0 ? ents[sel].name : NULL); break;
                }
                while (ui_key(&ui, &k)) if (k.key == KEY_ESC) mode = M_NONE;
            } else if (mode == M_NEWDIR || mode == M_RENAME) {
                int r = ui_prompt(&ui, mode == M_NEWDIR ? "New folder" : "Rename", mode == M_NEWDIR ? "Create" : "Rename", namebuf, sizeof namebuf);
                if (r == 1) { if (mode == M_NEWDIR) do_mkdir(namebuf); else do_rename(namebuf); }
                if (r) mode = M_NONE;
            } else if (mode == M_DELETE) {
                char t[160];
                snprintf(t, sizeof t, "Delete \"%s\"%s?", sel >= 0 ? ents[sel].name : "", sel >= 0 && ents[sel].dir ? " and everything in it" : "");
                int r = ui_confirm(&ui, "Delete", t, "Delete");
                if (r == 1) do_delete();
                if (r) mode = M_NONE;
            }
        }
        ui_end(&ui);
    } while (ui_poll(&ui, 1000));
    gui_window_destroy(w);
    return 0;
}
