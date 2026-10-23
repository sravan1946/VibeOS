/* desktop - icons for /home/Desktop on the wallpaper. One full-screen ARGB window at the bottom of the z-order
 * (WIN_FLAG_DESKTOP): where it is transparent you see the wallpaper and clicks fall through to the compositor's
 * menu. Icons drag (snapping to a grid, remembered in /home/.desktop-icons), double-click opens through
 * /etc/open.conf, files can be dropped on them from Files (DRAG_FILE + EV_DROP). Translucent things are drawn
 * as sprites: rendered on black and on white, alpha recovered from the difference (gfx can't draw into alpha). */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syscall.h>
#include <fsutil.h>
#include <design.h>
#include <icons.h>

#define DIR_PATH "/home/Desktop"
#define POS_FILE "/home/.desktop-icons"
#define CELL_W 96                 /* pitch; a cell is 84 x 84 (docs/DESIGN.md §9 Desktop icons) */
#define CELL_H 96
#define CELL 84
#define MARGIN SP_5
#define LABEL RGB(0xF2, 0xF2, 0xF2)   /* the wallpaper is dark in both themes */
#define MAXI 256
#define SPR_W 300
#define SPR_H 240

struct item { char name[96]; int kind, cx, cy; bool dir; };
static struct item ic[MAXI];
static int nic, sel = -1, hov = -1, rows = 1, cols = 1;
static bool dirty = true;
static uint32_t *tbk, *twh;

/* ---- sprites ---- */
static void sprite(struct ui *ui, int x, int y, int w, int h, void (*fn)(struct canvas *, void *), void *arg) {
    if (w > SPR_W || h > SPR_H) return;
    struct canvas a, b;
    canvas_init(&a, tbk, w, h, w);
    canvas_init(&b, twh, w, h, w);
    for (int i = 0; i < w * h; i++) { tbk[i] = RGB(0, 0, 0); twh[i] = RGB(255, 255, 255); }
    fn(&a, arg);
    fn(&b, arg);
    struct canvas *o = ui->c;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= o->h) continue;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= o->w) continue;
            uint32_t pb = tbk[j * w + i], pw = twh[j * w + i];
            int d = (((pw >> 16) & 255) - ((pb >> 16) & 255) + ((pw >> 8) & 255) - ((pb >> 8) & 255) + (pw & 255) - (pb & 255)) / 3;
            int al = 255 - d;
            if (al <= 0) continue;
            if (al > 255) al = 255;
            int r = (int)((pb >> 16) & 255) * 255 / al, g = (int)((pb >> 8) & 255) * 255 / al, bl = (int)(pb & 255) * 255 / al;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (bl > 255) bl = 255;
            uint32_t *p = &o->px[dy * o->stride + dx];
            int da = *p >> 24;
            if (!da || al == 255) { *p = (uint32_t)al << 24 | r << 16 | g << 8 | bl; continue; }
            int wd = da * (255 - al) / 255, oa = al + wd;
            int nr = (r * al + (int)((*p >> 16) & 255) * wd) / oa, ng = (g * al + (int)((*p >> 8) & 255) * wd) / oa, nb = (bl * al + (int)(*p & 255) * wd) / oa;
            *p = (uint32_t)oa << 24 | nr << 16 | ng << 8 | nb;
        }
    }
}

/* premultiplied ARGB artwork: d = s + d * (255 - a) / 255 */
static void blit_pm(struct canvas *c, int x, int y, const struct aicon *a) {
    if (!a) return;
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            int dx = x + i, dy = y + j;
            uint32_t p = a->argb[j * a->w + i], al = p >> 24;
            if (!al || dx < c->cx0 || dy < c->cy0 || dx >= c->cx1 || dy >= c->cy1) continue;
            uint32_t *d = &c->px[dy * c->stride + dx], k = 255 - al, q = *d;
            k += k >> 7;
            *d = 0xFF000000u | (((p & 0xFF00FF) + (((q & 0xFF00FF) * k >> 8) & 0xFF00FF)) + ((p & 0xFF00) + (((q & 0xFF00) * k >> 8) & 0xFF00)));
        }
}

/* the icon's drop shadow, 0 1 2 at 30%: its alpha blurred 3x3, one pixel down */
static void icon_shadow(struct canvas *c, int x, int y, const struct aicon *a) {
    if (!a) return;
    for (int j = -1; j <= a->h; j++)
        for (int i = -1; i <= a->w; i++) {
            int sum = 0;
            for (int v = -1; v <= 1; v++)
                for (int u = -1; u <= 1; u++) {
                    int sx = i + u, sy = j - 1 + v;
                    if (sx >= 0 && sy >= 0 && sx < a->w && sy < a->h) sum += a->argb[sy * a->w + sx] >> 24;
                }
            if (sum) gfx_pixel(c, x + i, y + j, RGBA(0, 0, 0, sum * 77 / (9 * 255)));
        }
}

/* file-type artwork; the standard folders get their emblem */
static int icon_for(const struct item *e) {
    static const struct { const char *name; int id; } folders[] = {
        { "Documents", FILEICON_FOLDER_DOCUMENTS }, { "Downloads", FILEICON_FOLDER_DOWNLOADS }, { "Music", FILEICON_FOLDER_MUSIC },
        { "Pictures", FILEICON_FOLDER_PICTURES }, { "Projects", FILEICON_FOLDER_PROJECTS }, { "Desktop", FILEICON_FOLDER_DESKTOP },
        { "Trash", FILEICON_FOLDER_TRASH },
    };
    const char *x = strrchr(e->name, '.');
    x = x ? x + 1 : "";
    switch (e->kind) {
    case FK_DIR:
        for (unsigned i = 0; i < sizeof folders / sizeof folders[0]; i++) if (!strcmp(e->name, folders[i].name)) return folders[i].id;
        return FILEICON_FOLDER;
    case FK_EXE: return FILEICON_EXEC;
    case FK_TEXT: return FILEICON_TEXT;
    case FK_CODE: return !strcmp(x, "sh") ? FILEICON_SCRIPT : FILEICON_CODE;
    case FK_IMAGE: return FILEICON_IMAGE;
    case FK_AUDIO: return FILEICON_AUDIO;
    case FK_HTML: return FILEICON_WEB;
    }
    return !strcmp(x, "zip") || !strcmp(x, "tar") || !strcmp(x, "gz") || !strcmp(x, "tgz") ? FILEICON_ARCHIVE : FILEICON_GENERIC;
}

/* a label with its 0 1 2 black-70% text shadow */
static void label(struct canvas *c, const struct font *f, int x, int y, int maxw, const char *s) {
    static const signed char off[4][3] = { { 0, 1, 140 }, { -1, 1, 45 }, { 1, 1, 45 }, { 0, 2, 60 } };
    for (int k = 0; k < 4; k++) gfx_text_ell(c, f, x + off[k][0], y + off[k][1], maxw, s, RGBA(0, 0, 0, off[k][2]));
    gfx_text_ell(c, f, x, y, maxw, s, LABEL);
}

struct cellarg { const struct item *i; bool sel, hov; };
static void draw_cell(struct canvas *c, void *p) {
    struct cellarg *a = p;
    if (a->sel) gfx_round_fill(c, 0, 0, CELL, CELL, R_2, ui_th.selection);
    else if (a->hov) gfx_round_fill(c, 0, 0, CELL, CELL, R_2, RGBA(255, 255, 255, 20));
    const struct aicon *ic = fileicon_get(icon_for(a->i), ICON_APP);
    int ix = (CELL - ICON_APP) / 2, iy = 6;
    icon_shadow(c, ix, iy, ic);
    blit_pm(c, ix, iy, ic);
    const char *s = a->i->name;
    int maxw = CELL - 2 * SP_2, w = gfx_text_width(T_BODY, s);
    label(c, T_BODY, SP_2 + (w < maxw ? (maxw - w) / 2 : 0), iy + ICON_APP + SP_2, maxw, s);
}

/* ---- the icon table ---- */
static int cmp_icon(const void *a, const void *b) {
    const struct item *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static bool taken(int cx, int cy, int except) {
    for (int i = 0; i < nic; i++) if (i != except && ic[i].cx == cx && ic[i].cy == cy) return true;
    return false;
}

static void nearest_free(int *cx, int *cy, int except) {       /* closest unoccupied cell to (cx, cy) */
    int bx = *cx, by = *cy, best = 1 << 30;
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
    if (*cx >= cols) *cx = cols - 1;
    if (*cy >= rows) *cy = rows - 1;
    for (int x = 0; x < cols + 8; x++)
        for (int y = 0; y < rows; y++) {
            if (taken(x, y, except)) continue;
            int d = (x - *cx) * (x - *cx) * 2 + (y - *cy) * (y - *cy);
            if (d < best) { best = d; bx = x; by = y; }
        }
    *cx = bx; *cy = by;
}

static void save_pos(void) {
    FILE *f = fopen(POS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < nic; i++) fprintf(f, "%d %d %s\n", ic[i].cx, ic[i].cy, ic[i].name);
    fclose(f);
}

static uint32_t dir_sig(void) {
    DIR *d = opendir(DIR_PATH);
    uint32_t h = 2166136261u;
    struct dirent *de;
    while (d && (de = readdir(d))) for (const char *s = de->d_name; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    if (d) closedir(d);
    return h;
}

static void rescan(void) {
    char oldsel[96] = "";
    if (sel >= 0 && sel < nic) snprintf(oldsel, sizeof oldsel, "%s", ic[sel].name);
    struct item old[MAXI];
    int nold = nic;
    memcpy(old, ic, sizeof(struct item) * nic);
    nic = 0; sel = hov = -1;
    DIR *d = opendir(DIR_PATH);
    struct dirent *de;
    while (d && (de = readdir(d)) && nic < MAXI) {
        if (de->d_name[0] == '.') continue;
        struct item *e = &ic[nic++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char p[PATH_MAX];
        snprintf(p, sizeof p, DIR_PATH "/%s", de->d_name);
        struct stat st;
        e->dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        e->kind = fs_kind(e->name, e->dir, stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111));
        e->cx = e->cy = -1;
    }
    if (d) closedir(d);
    qsort(ic, nic, sizeof *ic, cmp_icon);
    FILE *f = fopen(POS_FILE, "r");                              /* saved positions */
    char line[160];
    while (f && fgets(line, sizeof line, f)) {
        int cx, cy, n = 0;
        if (sscanf(line, "%d %d %n", &cx, &cy, &n) < 2 || !n) continue;
        line[strcspn(line, "\r\n")] = 0;
        for (int i = 0; i < nic; i++) if (ic[i].cx < 0 && !strcmp(ic[i].name, line + n)) { ic[i].cx = cx; ic[i].cy = cy; }
    }
    if (f) fclose(f);
    bool changed = nic != nold;
    for (int i = 0; i < nic; i++) {                             /* new files: first free cell, column by column */
        if (ic[i].cx >= 0 && !taken(ic[i].cx, ic[i].cy, i)) continue;
        int cx = 0, cy = 0;
        for (;; cy++) { if (cy >= rows) { cy = 0; cx++; } if (!taken(cx, cy, i)) break; }
        ic[i].cx = cx; ic[i].cy = cy;
        changed = true;
    }
    (void)old;
    for (int i = 0; i < nic; i++) if (!strcmp(ic[i].name, oldsel)) sel = i;
    if (changed) save_pos();
    dirty = true;
}

static void cell_xy(const struct item *e, int *x, int *y) { *x = MARGIN + e->cx * CELL_W; *y = MARGIN + e->cy * CELL_H; }
static int icon_at(int px, int py) {
    for (int i = 0; i < nic; i++) { int x, y; cell_xy(&ic[i], &x, &y); if (px >= x && py >= y && px < x + CELL && py < y + CELL) return i; }
    return -1;
}

static void open_icon(int i) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, DIR_PATH "/%s", ic[i].name);
    if (ic[i].dir) { char *a[3] = { "files", p, NULL }; spawnv("/bin/files", a); }
    else if (fs_open(p) < 0) gui_notify("Can't open", ic[i].name, "bad");
}

/* ---- context menu (docs/DESIGN.md §9 Menus) ---- */
static const char *mlab[] = { "Open", "Show in Files", "Move to Trash" };
static const short mico[] = { ICON_FOLDER_OPEN, ICON_FOLDER, ICON_TRASH };
#define MW 200
#define MH (2 * SP_2 + 3 * H_ROW + 2 * SP_2 + 1)
#define MPAD 24                                            /* room for the E2 shadow in the sprite */
static int mx0, my0, mhov = -1;
static bool menu_on;
static int m_iy(int i) { return SP_2 + i * H_ROW + (i == 2 ? 2 * SP_2 + 1 : 0); }
static void draw_menu(struct canvas *c, void *p) {
    int x = MPAD, y = MPAD;
    gfx_elev(c, x, y, MW, MH, R_3, E2, ui_th.elev[E2]);
    gfx_round_fill(c, x, y, MW, MH, R_3, ui_th.overlay);
    gfx_round_rect(c, x, y, MW, MH, R_3, ui_th.line);
    gfx_hline(c, x + SP_4, y + m_iy(2) - SP_2 - 1, MW - 2 * SP_4, ui_th.line);
    for (int i = 0; i < 3; i++) {
        int iy = y + m_iy(i);
        color_t fg = i == 2 ? ui_th.danger : ui_th.text;
        if (i == mhov) gfx_round_fill(c, x + SP_2, iy, MW - 2 * SP_2, H_ROW, R_2, ui_th.hover_ov);
        const struct icon *ic = icon_get(mico[i], ICON_SM);
        if (ic) gfx_mask(c, x + SP_4, iy + (H_ROW - ICON_SM) / 2, ic->a8, ic->w, ic->h, ic->w, i == 2 ? ui_th.danger : ui_th.text2);
        gfx_text(c, T_BODY, x + SP_4 + ICON_SM + SP_3, iy + (H_ROW - T_BODY->height) / 2, mlab[i], fg);
    }
}

static void menu_act(int a) {
    if (sel < 0) return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, DIR_PATH "/%s", ic[sel].name);
    if (a == 0) open_icon(sel);
    else if (a == 1) { char *av[3] = { "files", p, NULL }; spawnv("/bin/files", av); }
    else if (a == 2) { fs_trash_put(p, NULL, 0); rescan(); }
}

/* ---- drag and drop ---- */
static bool drop_into(const char *src, const char *dir, char *made) {
    char dst[PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s", dir, fs_base(src));
    fs_unique(dst, sizeof dst);
    int r = rename(src, dst);
    if (r && !fs_copy(src, dst)) r = fs_rmrf(src);
    if (made) snprintf(made, 96, "%s", fs_base(dst));
    return r == 0;
}

static void external_drop(int px, int py) {                      /* files from Files (or anywhere) dropped on the wallpaper */
    FILE *f = fopen(DRAG_FILE, "r");
    char b[PATH_MAX + 2], made[96];
    if (!f) return;
    int n = 0;
    int tgt = icon_at(px, py);
    if (fgets(b, sizeof b, f)) {
        int cx = (px - MARGIN) / CELL_W, cy = (py - MARGIN) / CELL_H;
        while (fgets(b, sizeof b, f)) {
            b[strcspn(b, "\r\n")] = 0;
            struct stat st;
            if (b[0] != '/' || stat(b, &st)) continue;
            char par[PATH_MAX];
            snprintf(par, sizeof par, "%s", b);
            *strrchr(par, '/') = 0;
            if (tgt >= 0 && ic[tgt].dir) {                       /* onto a folder icon: move into it */
                char fd[PATH_MAX];
                snprintf(fd, sizeof fd, DIR_PATH "/%s", ic[tgt].name);
                if (strcmp(par, fd)) drop_into(b, fd, NULL);
                continue;
            }
            if (!strcmp(par, DIR_PATH)) { rescan(); for (int i = 0; i < nic; i++) if (!strcmp(ic[i].name, fs_base(b))) { int x = cx, y = cy; nearest_free(&x, &y, i); ic[i].cx = x; ic[i].cy = y; } continue; }
            if (!drop_into(b, DIR_PATH, made)) continue;
            rescan();
            for (int i = 0; i < nic; i++) if (!strcmp(ic[i].name, made)) { int x = cx, y = cy; nearest_free(&x, &y, i); ic[i].cx = x; ic[i].cy = y; }
            n++;
        }
    }
    fclose(f);
    unlink(DRAG_FILE);
    save_pos();
    rescan();
    char t[40];
    snprintf(t, sizeof t, "%d item(s) moved to the desktop", n);
    if (n) gui_notify("Desktop", t, "good");
}

int main(void) {
    mkdir(DIR_PATH, 0755);
    struct gui_window *w = gui_window_create("Desktop", 0, 0, WIN_FLAG_DESKTOP | WIN_FLAG_FRAMELESS | WIN_FLAG_NORESIZE);
    if (!w) { perror("desktop"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    tbk = malloc(SPR_W * SPR_H * 4);
    twh = malloc(SPR_W * SPR_H * 4);
    if (!tbk || !twh) return 1;
    rows = (w->h - 2 * MARGIN) / CELL_H; cols = (w->w - 2 * MARGIN) / CELL_W;
    if (rows < 1) rows = 1;
    rescan();
    uint32_t sig = dir_sig(), last = uptime_ms();
    int press = -1, px0 = 0, py0 = 0, pend = 0, pend_x = 0, pend_y = 0;
    bool dragging = false;
    int gx = 0, gy = 0;
    do {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
        uint32_t now = uptime_ms();
        if (ui.resized) { rows = (w->h - 2 * MARGIN) / CELL_H; cols = (w->w - 2 * MARGIN) / CELL_W; if (rows < 1) rows = 1; rescan(); }
        if (now - last >= 1000) { last = now; uint32_t s = dir_sig(); if (s != sig) { sig = s; rescan(); } }
        int mx = ui.mx, my = ui.my;
        if (pend) {                                            /* our own drag ended: EV_DROP says it landed on the desktop */
            if (ui.dropped) {
                int tgt = icon_at(pend_x, pend_y);
                if (sel >= 0 && tgt >= 0 && tgt != sel && ic[tgt].dir) {            /* onto a folder */
                    char s[PATH_MAX], d[PATH_MAX];
                    snprintf(s, sizeof s, DIR_PATH "/%s", ic[sel].name);
                    snprintf(d, sizeof d, DIR_PATH "/%s", ic[tgt].name);
                    drop_into(s, d, NULL);
                    rescan();
                } else if (sel >= 0) {
                    int cx = (pend_x - MARGIN) / CELL_W, cy = (pend_y - MARGIN) / CELL_H;
                    if (pend_x < MARGIN) cx = 0;
                    if (pend_y < MARGIN) cy = 0;
                    nearest_free(&cx, &cy, sel);
                    ic[sel].cx = cx; ic[sel].cy = cy;
                    save_pos();
                }
                unlink(DRAG_FILE);
                pend = 0; dirty = true;
            } else if (!--pend) { unlink(DRAG_FILE); dirty = true; }     /* dropped on a window: it took the file */
        } else if (ui.dropped) { external_drop(ui.drop_x, ui.drop_y); dirty = true; }
        if (menu_on) {
            struct gui_event k;
            while (ui_key(&ui, &k)) {}
        }
        int h = -1;
        if (!menu_on) h = icon_at(mx, my);
        if (h != hov && !dragging) { hov = h; dirty = true; }
        if (ui.pressed) {
            if (menu_on) {
                int a = mhov;
                menu_on = false; dirty = true;
                if (a >= 0) menu_act(a);
            } else {
                int i = icon_at(mx, my);
                if (i != sel) { sel = i; dirty = true; }
                press = i; px0 = mx; py0 = my;
                if (i >= 0 && ui.dbl) { open_icon(i); press = -1; }
            }
        }
        if (ui.rpressed) {
            int i = menu_on ? -1 : icon_at(mx, my);
            if (i >= 0) { sel = i; menu_on = true; mx0 = mx; my0 = my; mhov = -1; }
            else menu_on = false;
            dirty = true;
        }
        if (menu_on) {
            int mxp = mx0 + MW > w->w ? mx0 - MW : mx0, myp = my0 + MH > w->h ? my0 - MH : my0, nh = -1;
            for (int i = 0; i < 3; i++) if (mx >= mxp && mx < mxp + MW && my >= myp + m_iy(i) && my < myp + m_iy(i) + H_ROW) nh = i;
            if (nh != mhov) { mhov = nh; dirty = true; }
        }
        if ((ui.buttons & 1) && press >= 0) {
            if (!dragging && abs(mx - px0) + abs(my - py0) > 8) {
                dragging = true;
                FILE *f = fopen(DRAG_FILE, "w");
                if (f) { fprintf(f, "move\n" DIR_PATH "/%s\n", ic[press].name); fclose(f); }
            }
            if (dragging) { gx = mx; gy = my; dirty = true; }
        }
        if (ui.released && press >= 0) {
            if (dragging) { pend = 6; pend_x = mx; pend_y = my; dirty = true; }
            dragging = false; press = -1;
        }
        if (dirty) {
            dirty = false;
            struct canvas *c = ui.c;
            memset(c->px, 0, (size_t)c->stride * c->h * 4);
            if (menu_on) for (int i = 0; i < c->stride * c->h; i++) c->px[i] = 0x02000000;   /* every click must reach us to close it */
            int drag_i = dragging ? press : -1;
            for (int i = 0; i < nic; i++) {
                if (i == drag_i) continue;
                int x, y;
                cell_xy(&ic[i], &x, &y);
                for (int j = y; j < y + CELL && j < c->h; j++) for (int k2 = x; k2 < x + CELL && k2 < c->w; k2++) if (!c->px[j * c->stride + k2]) c->px[j * c->stride + k2] = 0x02000000;
                struct cellarg a = { &ic[i], i == sel, i == hov };
                sprite(&ui, x, y, CELL, CELL, draw_cell, &a);
            }
            if (dragging && press >= 0) {
                struct cellarg a = { &ic[press], true, false };
                sprite(&ui, gx - CELL / 2, gy - CELL / 2, CELL, CELL, draw_cell, &a);
            }
            if (menu_on) {
                int mxp = mx0 + MW > w->w ? mx0 - MW : mx0, myp = my0 + MH > w->h ? my0 - MH : my0;
                sprite(&ui, mxp - MPAD, myp - MPAD, MW + 2 * MPAD, MH + 2 * MPAD, draw_menu, NULL);
            }
            ui_end(&ui);
        }
    } while (ui_poll(&ui, pend ? 60 : 1000));
    gui_window_destroy(w);
    return 0;
}
