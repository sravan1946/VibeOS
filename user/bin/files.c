/* files - file manager: icon grid + list views, sidebar, history, multi-select (click, Ctrl/Shift, rubber band),
 * clipboard, trash, drag and drop (within Files, to/from other windows and the desktop via DRAG_FILE), thumbnails */
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
#include <fsutil.h>
#include <fileicon.h>
#include "image.h"

#define MAX_ENT 2048
#define HIST_MAX 32
#define SIDE_W 168
#define TB_H 52
#define SB_H 28
#define HDR_H 28
#define LROW 28
#define CW 104
#define CH 100
#define TH 64
#define NTHUMB 64
#define TRASH_FILES "/home/.trash/files"
#define CLIP_HDR "vibe-files:"

struct ent {
    char name[96];
    char *orig;                       /* trash view: where it came from */
    bool dir, exe, sel, sel0;
    int kind;
    uint32_t size, mtime;             /* mtime = deletion time in the trash */
};

static struct ent *ents;
static int nents, *ord, nord;         /* ord: entries passing the search filter, in order */
static char cwd[PATH_MAX] = "/home";
static char hist[HIST_MAX][PATH_MAX];
static int nhist, hpos;
static char pathbox[PATH_MAX], qbox[64], qlast[64];
static int scroll, anchor = -1, cur = -1;
static int sort_key, sort_desc;       /* 0 name, 1 size, 2 modified */
static bool vgrid = true, hidden, intrash, relayout;
static int tz_min, mods, trash_n;
static uint32_t dir_sig, tick_ms;
static char msg[128];
static uint32_t msg_until;
static bool msg_bad;

enum { M_NONE, M_MENU, M_NEWDIR, M_NEWFILE, M_RENAME, M_DELETE, M_EMPTY, M_PROPS };
static int mode;
static char namebuf[96];
static int menu_x, menu_y;
enum { A_OPEN = 1, A_OPENTXT, A_CUT, A_COPY, A_PASTE, A_RENAME, A_TRASH, A_DELETE, A_PROPS, A_NEWDIR, A_NEWFILE, A_REFRESH,
       A_RESTORE, A_EMPTY, A_GOTRASH, A_SELALL, A_SORTN, A_SORTS, A_SORTM, A_HIDDEN, A_VIEW };
struct mi { const char *label; int id; bool sep, dis; };
static struct mi menu[18];
static int nmenu;

/* mouse state */
static int press_k = -1, press_x, press_y;       /* item pressed, and where */
static bool dragging, banding;
static int band_x, band_y;                       /* rubber band origin, content coordinates */
static int pend;                                 /* frames still waiting to learn whether a drop landed on us */
static int pend_x, pend_y;
static uint32_t drag_clear;

static void say(bool bad, const char *fmt, const char *arg) {
    snprintf(msg, sizeof msg, fmt, arg);
    msg_bad = bad;
    msg_until = uptime_ms() + 4000;
}

static void join(char *out, const char *dir, const char *name) { snprintf(out, PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name); }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool protected_path(const char *p) { return !p[1] || !strchr(p + 1, '/'); }      /* "/" and top-level folders */
static bool ctrl(void) { return mods & MOD_CTRL; }
static bool shift(void) { return mods & MOD_SHIFT; }

/* ---- thumbnails: decoded one per frame, scaled to fit TH x TH, LRU-cached ---- */
struct thumb { char path[PATH_MAX]; uint32_t mtime, size, use; uint32_t *px; int w, h; };
static struct thumb *thumbs;
static int nthumb;
static char want[PATH_MAX];
static bool thumb_busy;
static uint32_t want_mtime, want_size;

static struct thumb *thumb_get(const char *path, uint32_t mtime, uint32_t size) {
    for (int i = 0; i < nthumb; i++)
        if (thumbs[i].mtime == mtime && thumbs[i].size == size && !strcmp(thumbs[i].path, path)) { thumbs[i].use = tick_ms; return thumbs[i].px ? &thumbs[i] : NULL; }
    if (!want[0] && size <= (3u << 20)) { snprintf(want, sizeof want, "%s", path); want_mtime = mtime; want_size = size; }
    return NULL;
}

static bool thumb_make(void) {
    if (!want[0]) return false;
    if (!thumbs) thumbs = calloc(NTHUMB, sizeof *thumbs);
    if (!thumbs) { want[0] = 0; return false; }
    struct thumb *t = NULL;
    if (nthumb < NTHUMB) t = &thumbs[nthumb++];
    else {
        t = &thumbs[0];
        for (int i = 1; i < nthumb; i++) if (thumbs[i].use < t->use) t = &thumbs[i];
        free(t->px);
    }
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", want);
    t->mtime = want_mtime; t->size = want_size; t->use = tick_ms;
    struct image im;
    if (image_load(want, &im) == 0) {
        float sc = (float)TH / (im.w > im.h ? im.w : im.h);
        if (sc > 1) sc = 1;
        int tw = (int)(im.w * sc), th = (int)(im.h * sc);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        t->px = malloc((size_t)tw * th * 4);
        if (t->px) { image_sample(&im, t->px, tw, th, tw, 0, 0, sc, 1); t->w = tw; t->h = th; }
        image_free(&im);
    }
    want[0] = 0;
    return true;
}

/* ---- listing ---- */
static int cmp_ent(const void *a, const void *b) {
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    int r = 0;
    if (sort_key == 1) r = x->size < y->size ? -1 : x->size > y->size;
    else if (sort_key == 2) r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
    if (!r) r = strcasecmp(x->name, y->name);
    return sort_desc ? -r : r;
}

static bool match(const char *name, const char *q) {
    if (!q[0]) return true;
    size_t n = strlen(q);
    for (const char *p = name; *p; p++) if (!strncasecmp(p, q, n)) return true;
    return false;
}

static void build_order(void) {
    nord = 0;
    if (!ord) ord = malloc(MAX_ENT * sizeof *ord);
    for (int i = 0; i < nents; i++) if (match(ents[i].name, qbox)) ord[nord++] = i;
    snprintf(qlast, sizeof qlast, "%s", qbox);
    if (anchor >= nord) anchor = -1;
    if (cur >= nord) cur = -1;
}

static uint32_t sig_of(const char *dir) {                       /* cheap change detection: hash of the names */
    DIR *d = opendir(dir);
    uint32_t h = 2166136261u;
    struct dirent *de;
    while (d && (de = readdir(d))) for (const char *s = de->d_name; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    if (d) closedir(d);
    return h;
}

static void refresh(bool keepsel) {
    char (*sv)[96] = NULL;
    int nsv = 0;
    if (keepsel) {
        sv = malloc(256 * sizeof *sv);
        for (int i = 0; sv && i < nents && nsv < 256; i++) if (ents[i].sel) snprintf(sv[nsv++], 96, "%s", ents[i].name);
    }
    for (int i = 0; i < nents; i++) free(ents[i].orig);
    nents = 0;
    if (!ents) ents = malloc(MAX_ENT * sizeof *ents);
    DIR *d = opendir(cwd);
    if (!d) say(true, "Cannot open %s", cwd);
    struct dirent *de;
    while (d && (de = readdir(d)) && nents < MAX_ENT) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && !hidden) continue;
        struct ent *e = &ents[nents++];
        memset(e, 0, sizeof *e);
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
        e->kind = fs_kind(e->name, e->dir, e->exe);
        if (intrash) {
            char o[PATH_MAX];
            uint32_t when = 0;
            if (fs_trash_info(e->name, o, sizeof o, &when) == 0) { e->orig = strdup(o); e->mtime = when; }
        }
        for (int k = 0; k < nsv; k++) if (!strcmp(sv[k], e->name)) e->sel = true;
    }
    if (d) closedir(d);
    free(sv);
    qsort(ents, nents, sizeof *ents, cmp_ent);
    build_order();
    dir_sig = sig_of(cwd);
    trash_n = fs_trash_count();
    snprintf(pathbox, sizeof pathbox, "%s", cwd);
}

static void select_name(const char *name) {
    for (int i = 0; i < nents; i++) ents[i].sel = !strcmp(ents[i].name, name);
    for (int k = 0; k < nord; k++) if (ents[ord[k]].sel) anchor = cur = k;
}

static void navigate(const char *np) {
    snprintf(cwd, sizeof cwd, "%s", np);
    intrash = !strcmp(cwd, TRASH_FILES);
    scroll = 0; anchor = cur = -1;
    qbox[0] = 0;
    refresh(false);
}

static void go(const char *p, bool push) {
    char np[PATH_MAX];
    struct stat st;
    if (!realpath(p, np) || stat(np, &st)) { say(true, "No such folder: %s", p); snprintf(pathbox, sizeof pathbox, "%s", cwd); return; }
    if (!S_ISDIR(st.st_mode)) { say(true, "Not a folder: %s", p); snprintf(pathbox, sizeof pathbox, "%s", cwd); return; }
    if (!nhist) { snprintf(hist[nhist++], PATH_MAX, "%s", np); hpos = 0; }
    else if (push && strcmp(np, cwd)) {
        nhist = hpos + 1;
        if (nhist == HIST_MAX) { memmove(hist[0], hist[1], (HIST_MAX - 1) * PATH_MAX); nhist--; }
        snprintf(hist[nhist++], PATH_MAX, "%s", np);
        hpos = nhist - 1;
    }
    navigate(np);
}

static void go_up(void) {
    char p[PATH_MAX], keep[96] = "";
    snprintf(p, sizeof p, "%s", cwd);
    char *s = strrchr(p, '/');
    if (s) snprintf(keep, sizeof keep, "%s", s + 1);
    if (s == p) p[1] = 0; else if (s) *s = 0;
    if (!strcmp(p, cwd)) return;
    go(p, true);
    select_name(keep);
}
static void go_back(void) { if (hpos > 0) { hpos--; navigate(hist[hpos]); } }
static void go_fwd(void) { if (hpos < nhist - 1) { hpos++; navigate(hist[hpos]); } }

/* ---- selection ---- */
static int nsel(void) { int n = 0; for (int i = 0; i < nents; i++) n += ents[i].sel; return n; }
static void sel_clear(void) { for (int i = 0; i < nents; i++) ents[i].sel = false; }
static void sel_range(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    for (int k = 0; k < nord; k++) ents[ord[k]].sel = k >= a && k <= b;
}
static int first_sel(void) { for (int k = 0; k < nord; k++) if (ents[ord[k]].sel) return ord[k]; return -1; }

static int collect(char (*out)[PATH_MAX], int max) {           /* paths of the selection */
    int n = 0;
    for (int k = 0; k < nord && n < max; k++) if (ents[ord[k]].sel) join(out[n++], cwd, ents[ord[k]].name);
    return n;
}

/* ---- operations ---- */
static void open_sel(void) {
    int n = nsel(), started = 0;
    char p[PATH_MAX], nm[96] = "";
    for (int k = 0; k < nord; k++) {
        struct ent *e = &ents[ord[k]];
        if (!e->sel) continue;
        join(p, cwd, e->name);
        if (e->dir) { if (n == 1) { go(p, true); return; } continue; }
        if (started >= 8) break;
        if (fs_open(p) < 0) { say(true, "No program to open %s", e->name); return; }
        snprintf(nm, sizeof nm, "%s", e->name);
        started++;
    }
    if (started) say(false, started == 1 ? "Opening %s" : "Opening %s and more", nm);
}

static void open_with(const char *prog) {
    int k0 = first_sel();
    if (k0 < 0 || ents[k0].dir) return;
    char p[PATH_MAX];
    join(p, cwd, ents[k0].name);
    char *argv[3] = { (char *)fs_base(prog), p, NULL };
    int nul = open("/dev/null", O_RDWR);
    struct spawn_opts o = { { nul, nul, nul }, NULL, 0 };
    if (spawnve(prog, argv, environ, &o) < 0) say(true, "Could not start %s", prog);
    if (nul >= 0) close(nul);
}

static void clip_put(bool cut) {
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = collect(ps, 64);
    char *b = malloc(n * (PATH_MAX + 1) + 32);
    if (b && n) {
        int len = snprintf(b, 32, CLIP_HDR "%s\n", cut ? "cut" : "copy");
        for (int i = 0; i < n; i++) len += sprintf(b + len, "%s\n", ps[i]);
        gui_clip_set(b, len);
        char t[40];
        snprintf(t, sizeof t, "%d", n);
        say(false, cut ? "Cut %s item(s)" : "Copied %s item(s)", t);
    }
    free(b); free(ps);
}

/* move or copy path into dir under a fresh name; 0 on success */
static int put_into(const char *src, const char *dir, bool copy, char *made) {
    char dst[PATH_MAX], par[PATH_MAX];
    snprintf(par, sizeof par, "%s", src);
    char *sl = strrchr(par, '/');
    if (sl && sl != par) *sl = 0; else if (sl) sl[1] = 0;
    size_t sn = strlen(src);
    if (!strncmp(dir, src, sn) && (dir[sn] == '/' || !dir[sn])) return -1;                   /* into itself */
    if (!copy && !strcmp(par, dir)) return 1;                                                /* already there */
    join(dst, dir, fs_base(src));
    fs_unique(dst, sizeof dst);
    int r;
    if (copy) r = fs_copy(src, dst);
    else { r = rename(src, dst); if (r && !fs_copy(src, dst)) r = fs_rmrf(src); }
    if (made) snprintf(made, 96, "%s", fs_base(dst));
    return r ? -1 : 0;
}

static void paste_list(char (*ps)[PATH_MAX], int n, const char *dir, bool copy) {
    int ok = 0, bad = 0;
    char made[96] = "", t[16];
    for (int i = 0; i < n; i++) {
        int r = put_into(ps[i], dir, copy, made);
        if (r == 0) ok++; else if (r < 0) bad++;
    }
    snprintf(t, sizeof t, "%d", ok);
    if (bad) say(true, "%s: some items could not be moved", copy ? "Copy" : "Move");
    else if (ok) say(false, copy ? "Copied %s item(s)" : "Moved %s item(s)", t);
    refresh(false);
    if (ok == 1 && !strcmp(dir, cwd)) select_name(made);
}

static void clip_paste(void) {
    if (intrash) { say(true, "%s", "Can't paste into the Trash"); return; }
    int len = 0;
    char *c = gui_clip_get(&len);
    if (!c || strncmp(c, CLIP_HDR, strlen(CLIP_HDR))) { free(c); say(true, "%s", "Nothing to paste"); return; }
    bool cut = !strncmp(c + strlen(CLIP_HDR), "cut", 3);
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    int n = 0;
    char *s = strchr(c, '\n');
    while (ps && s && *++s && n < 64) {
        char *e = strchr(s, '\n');
        if (!e) break;
        *e = 0;
        if (s[0] == '/' && exists(s)) snprintf(ps[n++], PATH_MAX, "%s", s);
        s = e;
    }
    if (ps && n) { paste_list(ps, n, cwd, !cut); if (cut) gui_clip_set("", 0); }
    else say(true, "%s", "Nothing to paste");
    free(ps); free(c);
}

static void trash_sel(void) {
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = collect(ps, 64), ok = 0;
    for (int i = 0; i < n; i++) {
        if (protected_path(ps[i])) { say(true, "%s is protected", fs_base(ps[i])); continue; }
        if (fs_trash_put(ps[i], NULL, 0) == 0) ok++;
    }
    char t[16];
    snprintf(t, sizeof t, "%d", ok);
    if (ok) say(false, "Moved %s item(s) to the Trash", t);
    else if (n) say(true, "%s", "Could not move to the Trash");
    free(ps);
    int keep = cur;
    refresh(false);
    if (keep >= 0 && nord) anchor = cur = keep < nord ? keep : nord - 1;
}

static void delete_sel(void) {                          /* permanent */
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = collect(ps, 64), ok = 0;
    for (int i = 0; i < n; i++) {
        if (protected_path(ps[i])) { say(true, "%s is protected", fs_base(ps[i])); continue; }
        if (fs_rmrf(ps[i]) == 0) {
            ok++;
            if (intrash) { char inf[PATH_MAX]; snprintf(inf, sizeof inf, "%s/info/%s", fs_trash_dir, fs_base(ps[i])); unlink(inf); }
        }
    }
    char t[16];
    snprintf(t, sizeof t, "%d", ok);
    if (ok) say(false, "Deleted %s item(s)", t);
    else say(true, "%s", "Could not delete");
    free(ps);
    refresh(false);
}

static void restore_sel(void) {
    int ok = 0;
    char t[16], dest[PATH_MAX];
    for (int k = 0; k < nord; k++) if (ents[ord[k]].sel && fs_trash_restore(ents[ord[k]].name, dest, sizeof dest) == 0) ok++;
    snprintf(t, sizeof t, "%d", ok);
    if (ok) say(false, "Restored %s item(s)", t); else say(true, "%s", "Could not restore");
    refresh(false);
}

static void do_new(const char *name, bool dir) {
    if (!name[0] || strchr(name, '/')) { say(true, "%s", "Invalid name"); return; }
    char p[PATH_MAX];
    join(p, cwd, name);
    if (exists(p)) { say(true, "%s already exists", name); return; }
    bool ok;
    if (dir) ok = mkdir(p, 0755) == 0;
    else { int fd = open(p, O_WRONLY | O_CREAT, 0644); ok = fd >= 0; if (ok) close(fd); }
    if (!ok) { say(true, "Could not create %s", name); return; }
    say(false, "Created %s", name);
    refresh(false);
    select_name(name);
}

static void do_rename(const char *name) {
    int i = first_sel();
    if (i < 0) return;
    if (!name[0] || strchr(name, '/')) { say(true, "%s", "Invalid name"); return; }
    if (!strcmp(name, ents[i].name)) return;
    char a[PATH_MAX], b[PATH_MAX];
    join(a, cwd, ents[i].name);
    join(b, cwd, name);
    if (exists(b)) { say(true, "%s already exists", name); return; }
    if (rename(a, b)) { say(true, "Could not rename to %s", name); return; }
    say(false, "Renamed to %s", name);
    refresh(false);
    select_name(name);
}

/* ---- drag payload shared with the desktop and other Files windows (DRAG_FILE) ---- */
static void drag_write(void) {
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = collect(ps, 64);
    FILE *f = fopen(DRAG_FILE, "w");
    if (f) { fputs("move\n", f); for (int i = 0; i < n; i++) fprintf(f, "%s\n", ps[i]); fclose(f); }
    free(ps);
}

static int drag_read(char (*ps)[PATH_MAX], int max) {          /* paths from DRAG_FILE, then deletes it */
    FILE *f = fopen(DRAG_FILE, "r");
    int n = 0;
    char b[PATH_MAX + 2];
    if (!f) return 0;
    if (fgets(b, sizeof b, f)) {
        while (n < max && fgets(b, sizeof b, f)) {
            b[strcspn(b, "\r\n")] = 0;
            if (b[0] == '/' && exists(b)) snprintf(ps[n++], PATH_MAX, "%s", b);
        }
    }
    fclose(f);
    unlink(DRAG_FILE);
    return n;
}

/* ---- layout ---- */
struct lay { int x, y, w, h, rh, cols, vis, nrows; };
static struct lay L;

static void layout(int W, int H) {
    L.x = SIDE_W; L.w = W - SIDE_W;
    if (vgrid) {
        L.y = TB_H + 6; L.rh = CH;
        L.cols = (L.w - 24) / CW;
        if (L.cols < 1) L.cols = 1;
        L.x = SIDE_W + 10;
    } else { L.y = TB_H + HDR_H + 4; L.rh = LROW; L.cols = 1; }
    L.h = H - SB_H - L.y - 2;
    L.vis = L.h / L.rh;
    if (L.vis < 1) L.vis = 1;
    L.nrows = (nord + L.cols - 1) / L.cols;
    if (scroll > L.nrows - L.vis) scroll = L.nrows - L.vis;
    if (scroll < 0) scroll = 0;
}

static void item_rect(int k, int *x, int *y, int *w, int *h) {
    int r = k / L.cols, c = k % L.cols;
    *x = vgrid ? L.x + c * CW : SIDE_W; *y = L.y + (r - scroll) * L.rh;
    *w = vgrid ? CW : L.w; *h = L.rh;
}

static int item_at(int px, int py, int W) {
    if (py < L.y || py >= L.y + L.h || px < SIDE_W || px >= W - 14) return -1;
    int r = scroll + (py - L.y) / L.rh, c = vgrid ? (px - L.x) / CW : 0;
    if (px < L.x || c >= L.cols) return -1;
    int k = r * L.cols + c;
    return k >= 0 && k < nord ? k : -1;
}

static void ensure_visible(int k) {
    if (k < 0) return;
    int r = k / L.cols;
    if (r < scroll) scroll = r;
    if (r >= scroll + L.vis) scroll = r - L.vis + 1;
}

/* ---- icons for the sidebar and toolbar ---- */
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
static void icon_dir(struct canvas *c, int x, int y, color_t col) {
    gfx_round_fill(c, x + 1, y + 3, 8, 5, 2, col);
    gfx_round_fill(c, x + 1, y + 6, 18, 12, 3, col);
}
static void icon_bin(struct canvas *c, int x, int y, color_t col) {
    gfx_round_fill(c, x + 3, y + 5, 14, 3, 1, col);
    gfx_fill(c, x + 8, y + 2, 4, 3, col);
    gfx_round_fill(c, x + 4, y + 8, 12, 10, 2, col);
    for (int i = 0; i < 3; i++) gfx_fill(c, x + 7 + i * 3, y + 10, 1, 6, UI_PANEL);
}
static void icon_screen(struct canvas *c, int x, int y, color_t col) {
    gfx_round_fill(c, x + 1, y + 3, 18, 12, 2, col);
    gfx_round_fill(c, x + 3, y + 5, 14, 8, 1, UI_PANEL);
    gfx_fill(c, x + 7, y + 16, 6, 2, col);
}
static void icon_note(struct canvas *c, int x, int y, color_t col) {
    gfx_circle_fill(c, x + 7, y + 15, 3, col);
    gfx_fill(c, x + 9, y + 3, 2, 13, col);
    gfx_fill(c, x + 9, y + 3, 7, 3, col);
}
static void icon_pic(struct canvas *c, int x, int y, color_t col) {
    gfx_round_fill(c, x + 1, y + 3, 18, 14, 3, col);
    gfx_circle_fill(c, x + 13, y + 8, 2, UI_PANEL);
    gfx_line_aa(c, x + 3, y + 15, x + 8, y + 9, 2, UI_PANEL);
    gfx_line_aa(c, x + 8, y + 9, x + 12, y + 15, 2, UI_PANEL);
}
static void icon_code(struct canvas *c, int x, int y, color_t col) {
    gfx_line_aa(c, x + 7, y + 4, x + 2, y + 10, 2.2f, col);
    gfx_line_aa(c, x + 2, y + 10, x + 7, y + 16, 2.2f, col);
    gfx_line_aa(c, x + 13, y + 4, x + 18, y + 10, 2.2f, col);
    gfx_line_aa(c, x + 18, y + 10, x + 13, y + 16, 2.2f, col);
}
enum { I_HOME, I_SCREEN, I_DIR, I_PIC, I_NOTE, I_CODE, I_BIN, I_DRIVE, I_GEAR };
static void side_icon(struct canvas *c, int ic, int x, int y, color_t col) {
    switch (ic) {
    case I_HOME: icon_home(c, x, y, col); break;
    case I_SCREEN: icon_screen(c, x, y, col); break;
    case I_DIR: icon_dir(c, x, y, col); break;
    case I_PIC: icon_pic(c, x, y, col); break;
    case I_NOTE: icon_note(c, x, y, col); break;
    case I_CODE: icon_code(c, x, y, col); break;
    case I_BIN: icon_bin(c, x, y, col); break;
    case I_DRIVE: icon_drive(c, x, y, col); break;
    default: icon_gear(c, x, y, col); break;
    }
}

static const struct place { const char *label, *path; int ico; } places[] = {
    { "Home", "/home", I_HOME }, { "Desktop", "/home/Desktop", I_SCREEN }, { "Documents", "/home/Documents", I_DIR },
    { "Pictures", "/home/Pictures", I_PIC }, { "Music", "/home/Music", I_NOTE }, { "Projects", "/home/projects", I_CODE },
    { "Trash", TRASH_FILES, I_BIN }, { 0 }, { "Root", "/", I_DRIVE }, { "Programs", "/bin", I_DIR }, { "Config", "/etc", I_GEAR },
};
#define NPLACES ((int)(sizeof places / sizeof places[0]))
#define PLACE_H 30
static int place_y(int i) { return TB_H + 12 + i * PLACE_H + (i > 7 ? 6 : 0); }
static int place_at(int x, int y) {
    for (int i = 0; i < NPLACES; i++) if (places[i].label && x >= 8 && x < SIDE_W - 8 && y >= place_y(i) && y < place_y(i) + PLACE_H - 2) return i;
    return -1;
}

static void fmt_size(char *o, int n, uint64_t s) {
    if (s < 1024) snprintf(o, n, "%u B", (unsigned)s);
    else if (s < 1024 * 1024) snprintf(o, n, "%u.%u KiB", (unsigned)(s / 1024), (unsigned)(s % 1024 * 10 / 1024));
    else snprintf(o, n, "%u.%u MiB", (unsigned)(s >> 20), (unsigned)((s & 0xFFFFF) * 10 >> 20));
}

static void fmt_time(char *o, int n, uint32_t t) {
    if (!t) { snprintf(o, n, "--"); return; }
    time_t lt = (time_t)(t + tz_min * 60);
    struct tm tm;
    gmtime_r(&lt, &tm);
    snprintf(o, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void centered_fit(struct ui *ui, const struct font *f, int x, int y, int w, const char *s, color_t col) {
    int tw = gfx_text_width(f, s);
    ui_text_fit(ui, f, tw < w ? x + (w - tw) / 2 : x, y, w, s, col);
}

/* a grid cell's name: up to two lines */
static void cell_name(struct ui *ui, int x, int y, int w, const char *s, color_t col) {
    if (gfx_text_width(&font_ui, s) <= w) { centered_fit(ui, &font_ui, x, y, w, s, col); return; }
    int n = strlen(s);
    while (n > 1 && gfx_text_width_n(&font_ui, s, n) > w) n--;
    char l1[96];
    snprintf(l1, sizeof l1, "%.*s", n, s);
    centered_fit(ui, &font_ui, x, y, w, l1, col);
    centered_fit(ui, &font_ui, x, y + font_ui.height, w, s + n, col);
}

/* ---- properties ---- */
static struct {
    char name[96], type[40], where[PATH_MAX], size[64], when[40], perm[16], prog[96], orig[PATH_MAX];
    int kind, count;
} pr;

static void props_open(void) {
    memset(&pr, 0, sizeof pr);
    int n = nsel();
    uint32_t files = 0, dirs = 0, mt = 0;
    uint64_t bytes = 0;
    char p[PATH_MAX];
    int one = -1;
    for (int k = 0; k < nord; k++) {
        struct ent *e = &ents[ord[k]];
        if (!e->sel) continue;
        join(p, cwd, e->name);
        uint32_t f, d;
        uint64_t b;
        fs_du(p, &f, &d, &b);
        files += f; dirs += d; bytes += b;
        one = ord[k]; mt = e->mtime;
    }
    if (n == 0) {                                               /* the folder itself */
        snprintf(pr.name, sizeof pr.name, "%s", fs_base(cwd)[0] ? fs_base(cwd) : "/");
        snprintf(pr.type, sizeof pr.type, "Folder");
        snprintf(pr.where, sizeof pr.where, "%s", cwd);
        struct stat st;
        if (stat(cwd, &st) == 0) mt = st.st_mtime;
        fs_du(cwd, &files, &dirs, &bytes);
        if (dirs) dirs--;
        pr.kind = FK_DIR; pr.count = 1;
    } else if (n == 1) {
        struct ent *e = &ents[one];
        static const char *tn[] = { "Folder", "Program", "Text document", "Source code", "Image", "Audio", "Web page", "File" };
        snprintf(pr.name, sizeof pr.name, "%s", e->name);
        snprintf(pr.type, sizeof pr.type, "%s", tn[e->kind]);
        snprintf(pr.where, sizeof pr.where, "%s", cwd);
        pr.kind = e->kind; pr.count = 1;
        join(p, cwd, e->name);
        struct stat st;
        if (stat(p, &st) == 0) {
            int m = st.st_mode;
            for (int i = 0; i < 9; i++) pr.perm[i] = m & (0400 >> i) ? "rwxrwxrwx"[i] : '-';
        }
        if (!e->dir) {
            char prog[PATH_MAX];
            int r = fs_assoc(p, prog, sizeof prog);
            snprintf(pr.prog, sizeof pr.prog, "%s", r == 1 ? fs_base(prog) : r == 2 ? "(runs itself)" : "(none)");
        }
        if (e->orig) snprintf(pr.orig, sizeof pr.orig, "%s", e->orig);
    } else {
        snprintf(pr.name, sizeof pr.name, "%d items", n);
        snprintf(pr.type, sizeof pr.type, "Selection");
        snprintf(pr.where, sizeof pr.where, "%s", cwd);
        pr.kind = FK_FILE; pr.count = n;
    }
    char sz[32];
    fmt_size(sz, sizeof sz, bytes);
    if (dirs || n > 1 || n == 0) snprintf(pr.size, sizeof pr.size, "%s  (%u files, %u folders)", sz, (unsigned)files, (unsigned)dirs);
    else snprintf(pr.size, sizeof pr.size, "%s", sz);
    if (n <= 1) fmt_time(pr.when, sizeof pr.when, mt);
    mode = M_PROPS;
}

static void dim_box(struct ui *ui, int pw, int ph, int *ox, int *oy) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    gfx_fill(c, 0, 0, W, H, RGBA(0, 0, 0, 150));
    int px = (W - pw) / 2, py = (H - ph) / 2;
    gfx_shadow(c, px, py, pw, ph, 12, 14, 120);
    gfx_round_fill(c, px, py, pw, ph, 12, UI_PANEL);
    gfx_round_rect(c, px, py, pw, ph, 12, RGBA(255, 255, 255, 40));
    *ox = px; *oy = py;
}

static int props_draw(struct ui *ui) {
    int W = ui->win->w, pw = W - 20 < 520 ? W - 20 : 520, ph = 310, px, py;
    dim_box(ui, pw, ph, &px, &py);
    struct canvas *c = ui->c;
    fileicon(c, pr.kind, px + 24, py + 22, 56, pr.name);
    ui_text_fit(ui, &font_ui_bold, px + 96, py + 30, pw - 120, pr.name, UI_TEXT);
    ui_text_fit(ui, &font_ui, px + 96, py + 54, pw - 120, pr.type, UI_TEXT_DIM);
    gfx_fill(c, px + 20, py + 92, pw - 40, 1, UI_BORDER);
    const char *lab[] = { "Location", "Size", "Modified", "Permissions", "Opens with", "Original" };
    const char *val[] = { pr.where, pr.size, pr.when, pr.perm, pr.prog, pr.orig };
    int y = py + 106;
    for (int i = 0; i < 6; i++) {
        if (!val[i][0]) continue;
        gfx_text(c, &font_ui, px + 24, y, lab[i], UI_TEXT_DIM);
        ui_text_fit(ui, &font_ui, px + 130, y, pw - 154, val[i], UI_TEXT);
        y += 30;
    }
    int r = 0;
    if (ui_button(ui, px + pw - 120, py + ph - 52, 100, 34, "Close", UI_ACCENT_BTN)) r = 1;
    struct gui_event k;
    while (ui_key(ui, &k)) if (k.key == KEY_ESC || k.key == KEY_ENTER) r = 1;
    return r;
}

/* ---- context menu ---- */
static void menu_add(const char *label, int id, bool dis) { menu[nmenu++] = (struct mi){ label, id, false, dis }; }
static void menu_sep(void) { menu[nmenu++] = (struct mi){ 0, 0, true, false }; }

static void menu_open_at(int x, int y, int what) {             /* what: 0 background, 1 item(s), 2 trash place */
    nmenu = 0;
    int n = nsel();
    if (what == 2) { menu_add("Open", A_GOTRASH, false); menu_add("Empty Trash", A_EMPTY, trash_n == 0); }
    else if (what == 1) {
        menu_add("Open", A_OPEN, false);
        if (n == 1 && first_sel() >= 0 && !ents[first_sel()].dir) menu_add("Open With Text Editor", A_OPENTXT, false);
        menu_sep();
        if (intrash) { menu_add("Restore", A_RESTORE, false); menu_add("Delete Permanently", A_DELETE, false); }
        else {
            menu_add("Cut", A_CUT, false); menu_add("Copy", A_COPY, false);
            menu_sep();
            menu_add("Rename", A_RENAME, n != 1);
            menu_add("Move to Trash", A_TRASH, false);
        }
        menu_sep();
        menu_add("Properties", A_PROPS, false);
    } else {
        if (intrash) { menu_add("Empty Trash", A_EMPTY, nents == 0); menu_sep(); }
        else {
            menu_add("New Folder", A_NEWDIR, false); menu_add("New Text File", A_NEWFILE, false);
            menu_add("Paste", A_PASTE, false);
            menu_sep();
        }
        menu_add("Select All", A_SELALL, false);
        menu_add(vgrid ? "View as List" : "View as Icons", A_VIEW, false);
        menu_add("Sort by Name", A_SORTN, false); menu_add("Sort by Size", A_SORTS, false); menu_add("Sort by Date", A_SORTM, false);
        menu_add(hidden ? "Hide Hidden Files" : "Show Hidden Files", A_HIDDEN, false);
        menu_add("Refresh", A_REFRESH, false);
        menu_sep();
        menu_add("Properties", A_PROPS, false);
    }
    menu_x = x; menu_y = y;
    mode = M_MENU;
}

static int menu_h(void) { int h = 12; for (int i = 0; i < nmenu; i++) h += menu[i].sep ? 9 : 30; return h; }

static void do_action(int a) {
    switch (a) {
    case A_OPEN: open_sel(); break;
    case A_GOTRASH: go(TRASH_FILES, true); break;
    case A_OPENTXT: open_with("/bin/notepad"); break;
    case A_CUT: clip_put(true); break;
    case A_COPY: clip_put(false); break;
    case A_PASTE: clip_paste(); break;
    case A_RENAME: { int i = first_sel(); if (i >= 0) { snprintf(namebuf, sizeof namebuf, "%s", ents[i].name); mode = M_RENAME; } break; }
    case A_TRASH: trash_sel(); break;
    case A_DELETE: if (nsel()) mode = M_DELETE; break;
    case A_PROPS: props_open(); break;
    case A_NEWDIR: namebuf[0] = 0; mode = M_NEWDIR; break;
    case A_NEWFILE: snprintf(namebuf, sizeof namebuf, "untitled.txt"); mode = M_NEWFILE; break;
    case A_REFRESH: refresh(true); break;
    case A_RESTORE: restore_sel(); break;
    case A_EMPTY: if (trash_n) mode = M_EMPTY; break;
    case A_SELALL: for (int k = 0; k < nord; k++) ents[ord[k]].sel = true; break;
    case A_SORTN: case A_SORTS: case A_SORTM: {
        int k = a - A_SORTN;
        if (sort_key == k) sort_desc = !sort_desc; else { sort_key = k; sort_desc = 0; }
        refresh(true);
        break;
    }
    case A_HIDDEN: hidden = !hidden; refresh(true); break;
    case A_VIEW: vgrid = !vgrid; scroll = 0; relayout = true; break;
    }
}

/* ---- drag and drop target (inside the window): a folder item, a sidebar place, else the current folder ---- */
static void drop_internal(int px, int py, int W, bool copy) {
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = collect(ps, 64);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", cwd);
    int pl = place_at(px, py), k = item_at(px, py, W);
    bool trash = false;
    if (pl >= 0) { if (!strcmp(places[pl].path, TRASH_FILES)) trash = true; else snprintf(dir, sizeof dir, "%s", places[pl].path); }
    else if (k >= 0 && ents[ord[k]].dir && !ents[ord[k]].sel) join(dir, cwd, ents[ord[k]].name);
    if (trash && !intrash) { trash_sel(); }
    else if (!trash && n) paste_list(ps, n, dir, copy);
    free(ps);
}

static void drop_external(int px, int py, int W) {              /* something was dropped on us from another window */
    char (*ps)[PATH_MAX] = malloc(64 * PATH_MAX);
    if (!ps) return;
    int n = drag_read(ps, 64);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", cwd);
    int pl = place_at(px, py), k = item_at(px, py, W);
    if (pl >= 0 && strcmp(places[pl].path, TRASH_FILES)) snprintf(dir, sizeof dir, "%s", places[pl].path);
    else if (k >= 0 && ents[ord[k]].dir) join(dir, cwd, ents[ord[k]].name);
    if (n && !intrash) paste_list(ps, n, dir, false);
    free(ps);
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Files", 920, 600, 0);
    if (!w) { perror("files"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    FILE *f = fopen("/etc/tz", "r");
    if (f) { char b[16] = { 0 }; if (fgets(b, sizeof b, f)) tz_min = atoi(b); fclose(f); }
    char start[PATH_MAX] = "/home", sel_name[96] = "";
    if (argc > 1) {
        snprintf(start, sizeof start, "%s", argv[1]);
        if (!is_dir(argv[1])) {                                      /* a file: show its folder with it selected */
            char rp[PATH_MAX];
            if (realpath(argv[1], rp)) {
                char *s = strrchr(rp, '/');
                snprintf(sel_name, sizeof sel_name, "%s", s + 1);
                if (s == rp) s[1] = 0; else *s = 0;
                snprintf(start, sizeof start, "%s", rp);
            }
        }
    }
    go(start, true);
    if (sel_name[0]) select_name(sel_name);
    uint32_t last_tick = uptime_ms();
    bool path_fresh = false;
    do {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}              /* reap apps we launched */
        tick_ms = uptime_ms();
        int W = w->w, H = w->h;
        bool modal = mode != M_NONE;
        if (!ui.focused) mods = 0;
        for (int i = 0; i < ui.nkeys; i++) {                     /* modifier state for clicks */
            mods = ui.keys[i].mods;
            int kc = ui.keys[i].key;
            if (kc == KEY_LCTRL || kc == KEY_RCTRL) mods |= MOD_CTRL;
            if (kc == KEY_LSHIFT || kc == KEY_RSHIFT) mods |= MOD_SHIFT;
            if (kc == KEY_LALT || kc == KEY_RALT) mods |= MOD_ALT;
        }
        for (int i = 0; i < ui.nups; i++) {
            int kc = ui.ups[i];
            if (kc == KEY_LCTRL || kc == KEY_RCTRL) mods &= ~MOD_CTRL;
            if (kc == KEY_LSHIFT || kc == KEY_RSHIFT) mods &= ~MOD_SHIFT;
            if (kc == KEY_LALT || kc == KEY_RALT) mods &= ~MOD_ALT;
        }
        if (tick_ms - last_tick >= 1000) {                       /* notice changes made by other programs */
            last_tick = tick_ms;
            if (sig_of(cwd) != dir_sig && !dragging && !banding) refresh(true);
            int tn = fs_trash_count();
            if (tn != trash_n) { trash_n = tn; if (intrash) refresh(true); }
        }
        if (drag_clear && tick_ms >= drag_clear) { drag_clear = 0; unlink(DRAG_FILE); }
        if (strcmp(qbox, qlast)) { build_order(); scroll = 0; }
        layout(W, H);

        /* a drag we started ended: did it land on us (EV_DROP follows the button release)? */
        if (pend) {
            if (ui.dropped && pend_x >= 0 && pend_y >= 0 && pend_x < W && pend_y < H) { drop_internal(pend_x, pend_y, W, ctrl()); unlink(DRAG_FILE); pend = 0; }
            else if (!--pend) drag_clear = tick_ms + 2000;              /* it went elsewhere: the target consumes the file */
        } else if (ui.dropped) drop_external(ui.drop_x, ui.drop_y, W);

        /* hide input from the background while a dialog or menu is up */
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, sd = ui.dbl, srp = ui.rpressed;
        int nk = ui.nkeys, ssc = ui.scroll;
        struct gui_event saved[32];
        memcpy(saved, ui.keys, sizeof saved);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.dbl = ui.rpressed = false; ui.nkeys = 0; ui.scroll = 0; }

        int tbW = W - 134 - 286 < 120 ? 120 : W - 134 - 286;
        int sx = 134 + tbW + 10;
        uint32_t path_id = ui_id(134, 8, tbW, 36), search_id = ui_id(sx, 8, 160, 36);
        bool typing = ui.focus == path_id || ui.focus == search_id;
        if (ui.focus == path_id && !path_fresh) { path_fresh = true; }
        if (ui.focus != path_id) path_fresh = false;
        struct gui_event k;

        /* ---- keyboard ---- */
        if (!typing) {
            while (!modal && ui_key(&ui, &k)) {
                int kc = k.key, m = k.mods;
                int n = nord, c0 = cur < 0 ? 0 : cur, step = vgrid ? L.cols : 1, nc = -1;
                bool lc = (kc | 0x20) >= 'a' && (kc | 0x20) <= 'z';
                if (kc == KEY_LCTRL || kc == KEY_RCTRL || kc == KEY_LSHIFT || kc == KEY_RSHIFT || kc == KEY_LALT || kc == KEY_RALT) continue;
                if ((m & MOD_ALT) && kc == KEY_LEFT) go_back();
                else if ((m & MOD_ALT) && kc == KEY_RIGHT) go_fwd();
                else if ((m & MOD_ALT) && kc == KEY_UP) go_up();
                else if (kc == KEY_DOWN) nc = cur < 0 ? 0 : c0 + step < n ? c0 + step : (c0 / L.cols < (n - 1) / L.cols ? n - 1 : c0);
                else if (kc == KEY_UP) nc = cur < 0 ? 0 : c0 - step >= 0 ? c0 - step : c0;
                else if (kc == KEY_RIGHT && vgrid) nc = cur < 0 ? 0 : c0 + 1 < n ? c0 + 1 : c0;
                else if (kc == KEY_LEFT && vgrid) nc = cur < 0 ? 0 : c0 > 0 ? c0 - 1 : 0;
                else if (kc == KEY_PGDN) nc = c0 + L.vis * L.cols < n ? c0 + L.vis * L.cols : n - 1;
                else if (kc == KEY_PGUP) nc = c0 - L.vis * L.cols > 0 ? c0 - L.vis * L.cols : 0;
                else if (kc == KEY_HOME) nc = 0;
                else if (kc == KEY_END) nc = n - 1;
                else if (kc == KEY_ENTER) open_sel();
                else if (kc == KEY_BACKSPACE) go_up();
                else if (kc == KEY_ESC) { if (qbox[0]) qbox[0] = 0; else sel_clear(); }
                else if (kc == KEY_DELETE) { if (nsel()) { if ((m & MOD_SHIFT) || intrash || fs_in_trash(cwd)) mode = M_DELETE; else trash_sel(); } }
                else if (kc == KEY_F2) do_action(A_RENAME);
                else if (kc == KEY_F5) refresh(true);
                else if ((m & MOD_CTRL) && lc) {
                    switch (kc | 0x20) {
                    case 'a': do_action(A_SELALL); break;
                    case 'c': clip_put(false); break;
                    case 'x': if (!intrash) clip_put(true); break;
                    case 'v': clip_paste(); break;
                    case 'n': do_action(A_NEWDIR); break;
                    case 'h': do_action(A_HIDDEN); break;
                    case 'l': ui.focus = path_id; path_fresh = true; break;
                    case 'f': ui.focus = search_id; break;
                    case 'i': do_action(A_PROPS); break;
                    case 'r': refresh(true); break;
                    }
                } else if (k.ch > ' ' && k.ch < 127 && !(m & (MOD_CTRL | MOD_ALT))) {   /* type to jump */
                    int from = cur < 0 ? 0 : cur + 1;
                    for (int j = 0; j < n; j++) {
                        int q = (from + j) % n;
                        if ((ents[ord[q]].name[0] | 0x20) == (k.ch | 0x20)) { nc = q; break; }
                    }
                }
                if (nc >= 0 && n) {
                    if (nc >= n) nc = n - 1;
                    if ((m & MOD_SHIFT) && anchor >= 0) sel_range(anchor, nc);
                    else if (!(m & MOD_CTRL)) { sel_clear(); ents[ord[nc]].sel = true; anchor = nc; }
                    cur = nc;
                    ensure_visible(nc);
                }
            }
        } else if (ui.focus == search_id) {                               /* Esc in the box clears it */
            for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i].key == KEY_ESC) { qbox[0] = 0; ui.focus = 0; ui.nkeys = 0; break; }
        } else if (ui.focus == path_id && path_fresh) {
            for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i].key == KEY_ESC) { snprintf(pathbox, sizeof pathbox, "%s", cwd); ui.focus = 0; ui.nkeys = 0; break; }                  /* typing over a freshly focused path replaces it */
            for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i].ch >= 32 && ui.keys[i].ch < 127 && !(ui.keys[i].mods & MOD_CTRL)) { pathbox[0] = 0; path_fresh = false; break; }
        }
        layout(W, H);

        /* ---- mouse in the content area ---- */
        int mx = ui.mx, my = ui.my;
        bool in_c = mx >= SIDE_W && mx < W - 14 && my >= L.y - (vgrid ? 0 : HDR_H + 4) && my < H - SB_H;
        if (!modal && ui_hover(&ui, SIDE_W, L.y, L.w, L.h) && ui.scroll) { scroll += ui.scroll * (vgrid ? 1 : 3); layout(W, H); }
        if (ui.pressed && in_c && my >= L.y && !modal) {
            int hit = item_at(mx, my, W);
            press_k = -1; dragging = banding = false;
            if (hit >= 0) {
                struct ent *e = &ents[ord[hit]];
                if (ctrl()) e->sel = !e->sel;
                else if (shift() && anchor >= 0) sel_range(anchor, hit);
                else if (!e->sel) { sel_clear(); e->sel = true; }
                if (!shift()) anchor = hit;
                cur = hit;
                if (ui.dbl && !ctrl() && !shift()) { sel_clear(); e->sel = true; open_sel(); press_k = -1; }
                else { press_k = hit; press_x = mx; press_y = my; }
            } else {
                if (!ctrl()) sel_clear();
                for (int i = 0; i < nents; i++) ents[i].sel0 = ents[i].sel;
                banding = true; band_x = mx - L.x; band_y = my - L.y + scroll * L.rh;
                anchor = cur = -1;
            }
        }
        if (ui.rpressed && !modal && ui_hover(&ui, SIDE_W, TB_H, L.w, H - TB_H - SB_H)) {
            int hit = my >= L.y ? item_at(mx, my, W) : -1;
            if (hit >= 0) { if (!ents[ord[hit]].sel) { sel_clear(); ents[ord[hit]].sel = true; } anchor = cur = hit; menu_open_at(mx, my, 1); }
            else { sel_clear(); menu_open_at(mx, my, 0); }
        }
        if (ui.rpressed && !modal && mx < SIDE_W && my > TB_H) {
            int pl = place_at(mx, my);
            if (pl >= 0 && !strcmp(places[pl].path, TRASH_FILES)) menu_open_at(mx, my, 2);
        }
        if ((ui.buttons & 1) && !modal) {
            if (press_k >= 0 && !dragging && abs(mx - press_x) + abs(my - press_y) > 8) { dragging = true; drag_write(); }
            if (banding) {
                int bx0 = band_x < mx - L.x ? band_x : mx - L.x, bx1 = band_x < mx - L.x ? mx - L.x : band_x;
                int cy = my - L.y + scroll * L.rh, by0 = band_y < cy ? band_y : cy, by1 = band_y < cy ? cy : band_y;
                for (int j = 0; j < nord; j++) {
                    int r = j / L.cols, c = j % L.cols;
                    int ix = vgrid ? c * CW + 8 : 0, iy = r * L.rh, iw = vgrid ? CW - 16 : L.w, ih = L.rh;
                    bool hit = ix < bx1 && ix + iw > bx0 && iy < by1 && iy + ih > by0;
                    ents[ord[j]].sel = ents[ord[j]].sel0 || hit;
                }
            }
        }
        if (ui.released) {
            if (dragging) {
                dragging = false;
                pend = 6; pend_x = mx; pend_y = my;
            } else if (press_k >= 0 && !ctrl() && !shift() && nsel() > 1) {
                int k0 = press_k;
                sel_clear();
                if (k0 < nord) ents[ord[k0]].sel = true;
            }
            press_k = -1; banding = false;
        }

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        gfx_fill(c, 0, 0, W, TB_H, UI_PANEL);
        gfx_fill(c, 0, TB_H - 1, W, 1, UI_BORDER);
        color_t dim = UI_TEXT_DIM;
        if (ui_tool(&ui, 10, 8, 36, 36, "", false)) go_back();
        color_t cb = hpos > 0 ? UI_TEXT : dim;
        gfx_line_aa(c, 36, 26, 20, 26, 2.2f, cb); gfx_line_aa(c, 20, 26, 26, 20, 2.2f, cb); gfx_line_aa(c, 20, 26, 26, 32, 2.2f, cb);
        if (ui_tool(&ui, 50, 8, 36, 36, "", false)) go_fwd();
        color_t cf = hpos < nhist - 1 ? UI_TEXT : dim;
        gfx_line_aa(c, 62, 26, 78, 26, 2.2f, cf); gfx_line_aa(c, 78, 26, 72, 20, 2.2f, cf); gfx_line_aa(c, 78, 26, 72, 32, 2.2f, cf);
        if (ui_tool(&ui, 90, 8, 36, 36, "", false)) go_up();
        gfx_line_aa(c, 108, 34, 108, 18, 2.2f, UI_TEXT); gfx_line_aa(c, 108, 18, 102, 24, 2.2f, UI_TEXT); gfx_line_aa(c, 108, 18, 114, 24, 2.2f, UI_TEXT);
        if (ui_textbox(&ui, 134, 8, tbW, 36, pathbox, sizeof pathbox, "path")) { go(pathbox, true); ui.focus = 0; }
        if (ui_textbox(&ui, sx, 8, 160, 36, qbox, sizeof qbox, "Search")) ui.focus = 0;
        int vx = sx + 168;
        if (ui_tool(&ui, vx, 8, 34, 36, "", vgrid)) { vgrid = true; scroll = 0; relayout = true; }
        for (int i = 0; i < 4; i++) gfx_fill(c, vx + 9 + (i & 1) * 9, 15 + (i >> 1) * 9, 7, 7, UI_TEXT);
        if (ui_tool(&ui, vx + 34, 8, 34, 36, "", !vgrid)) { vgrid = false; scroll = 0; relayout = true; }
        for (int i = 0; i < 3; i++) { gfx_fill(c, vx + 34 + 8, 16 + i * 7, 4, 3, UI_TEXT); gfx_fill(c, vx + 34 + 15, 16 + i * 7, 11, 3, UI_TEXT); }
        if (ui_tool(&ui, vx + 72, 8, 34, 36, "", false) && !intrash) { namebuf[0] = 0; mode = M_NEWDIR; ui.focus = 0; }
        gfx_fill(c, vx + 72 + 17 - 1, 17, 3, 18, UI_TEXT); gfx_fill(c, vx + 72 + 9, 25, 18, 3, UI_TEXT);

        /* sidebar */
        gfx_fill(c, 0, TB_H, SIDE_W, H - TB_H - SB_H, RGB(0x1A, 0x19, 0x28));
        gfx_fill(c, SIDE_W - 1, TB_H, 1, H - TB_H - SB_H, UI_BORDER);
        int dpl = dragging ? place_at(mx, my) : -1;
        for (int i = 0; i < NPLACES; i++) {
            if (!places[i].label) { gfx_fill(c, 14, place_y(i) + 11, SIDE_W - 28, 1, UI_BORDER); continue; }
            int y = place_y(i);
            bool act = !strcmp(cwd, places[i].path), hov = ui_hover(&ui, 8, y, SIDE_W - 16, PLACE_H - 2);
            if (i == dpl) gfx_round_rect(c, 8, y, SIDE_W - 16, PLACE_H - 2, 8, UI_ACCENT);
            if (act) gfx_round_fill(c, 8, y, SIDE_W - 16, PLACE_H - 2, 8, RGBA(0xB3, 0x88, 0xFF, 80));
            else if (hov || i == dpl) gfx_round_fill(c, 8, y, SIDE_W - 16, PLACE_H - 2, 8, RGBA(255, 255, 255, 18));
            side_icon(c, places[i].ico, 16, y + 5, act ? UI_ACCENT : UI_TEXT_DIM);
            gfx_text(c, &font_ui, 44, y + 5, places[i].label, UI_TEXT);
            if (places[i].ico == I_BIN && trash_n) {
                char b[12];
                snprintf(b, sizeof b, "%d", trash_n);
                int bw = gfx_text_width(&font_ui, b) + 12;
                gfx_round_fill(c, SIDE_W - 18 - bw, y + 5, bw, 18, 9, RGBA(0xB3, 0x88, 0xFF, 120));
                gfx_text_center(c, &font_ui, SIDE_W - 18 - bw, y + 5 + (18 - font_ui.height) / 2, bw, b, UI_TEXT);
            }
            if (ui.pressed && hov && !modal) go(places[i].path, true);
        }

        /* content */
        char tmp[PATH_MAX];
        struct canvas cc = *c;
        canvas_clip(&cc, SIDE_W, L.y, L.w, L.h);
        if (relayout) { layout(W, H); relayout = false; }
        int lcol_size = W - 190 - (intrash ? 260 : 100), lcol_mod = W - 190;
        if (!vgrid) {
            static const char *hn[] = { "Name", "Size", "Modified" };
            for (int hi = 0; hi < 3; hi++) {
                int hx = hi == 0 ? SIDE_W + 54 : hi == 1 ? lcol_size : lcol_mod;
                const char *label = intrash ? (hi == 0 ? "Name" : hi == 1 ? "Original location" : "Deleted") : hn[hi];
                int hw = gfx_text_width(&font_ui_bold, label);
                bool hov = ui_hover(&ui, hx - 6, TB_H, hw + 40, HDR_H);
                gfx_text(c, &font_ui_bold, hx, TB_H + 9, label, hov ? UI_TEXT : UI_TEXT_DIM);
                if (sort_key == hi) {                                   /* sort arrow */
                    int ax = hx + hw + 10, ay = TB_H + 14;
                    if (sort_desc) { gfx_line_aa(c, ax - 4, ay - 2, ax, ay + 3, 1.8f, UI_ACCENT); gfx_line_aa(c, ax, ay + 3, ax + 4, ay - 2, 1.8f, UI_ACCENT); }
                    else { gfx_line_aa(c, ax - 4, ay + 3, ax, ay - 2, 1.8f, UI_ACCENT); gfx_line_aa(c, ax, ay - 2, ax + 4, ay + 3, 1.8f, UI_ACCENT); }
                }
                if (ui.pressed && hov && !modal && !(hi == 1 && intrash)) { do_action(A_SORTN + hi); }
            }
            gfx_fill(c, SIDE_W + 8, TB_H + HDR_H, L.w - 16, 1, UI_BORDER);
        }
        if (!nord) gfx_text_center(&cc, &font_ui, SIDE_W, L.y + 40, L.w, nents ? "No matches" : intrash ? "The Trash is empty" : "This folder is empty", UI_TEXT_DIM);
        for (int j = scroll * L.cols; j < nord && j < (scroll + L.vis + 1) * L.cols; j++) {
            int x, y, iw, ih;
            item_rect(j, &x, &y, &iw, &ih);
            struct ent *e = &ents[ord[j]];
            bool hov = !modal && ui_hover(&ui, x, y, iw, ih) && mx < W - 14 && my >= L.y && my < L.y + L.h;
            bool dt = dragging && hov && e->dir && !e->sel;              /* a folder to drop onto */
            if (vgrid) {
                if (e->sel) { gfx_round_fill(&cc, x + 4, y + 2, CW - 8, CH - 4, 10, RGBA(0xB3, 0x88, 0xFF, 85)); gfx_round_rect(&cc, x + 4, y + 2, CW - 8, CH - 4, 10, RGBA(0xD8, 0xC4, 0xFF, 120)); }
                else if (hov) gfx_round_fill(&cc, x + 4, y + 2, CW - 8, CH - 4, 10, RGBA(255, 255, 255, 16));
                if (dt) gfx_round_rect(&cc, x + 4, y + 2, CW - 8, CH - 4, 10, UI_ACCENT);
                int ix = x + (CW - TH) / 2, iy = y + 8;
                struct thumb *t = NULL;
                if (e->kind == FK_IMAGE && !intrash) { join(tmp, cwd, e->name); t = thumb_get(tmp, e->mtime, e->size); }
                if (t) {
                    gfx_round_fill(&cc, ix + (TH - t->w) / 2 - 2, iy + (TH - t->h) / 2 - 2, t->w + 4, t->h + 4, 4, RGBA(255, 255, 255, 40));
                    gfx_blit(&cc, ix + (TH - t->w) / 2, iy + (TH - t->h) / 2, t->px, t->w, t->h, t->w);
                } else fileicon(&cc, e->kind, x + (CW - 56) / 2, iy + 4, 56, e->name);
                cell_name(&ui, x + 8, y + 8 + TH + 4, CW - 16, e->name, UI_TEXT);
            } else {
                if (e->sel) gfx_round_fill(&cc, x + 6, y + 1, iw - 18, ih - 2, 7, RGBA(0xB3, 0x88, 0xFF, 85));
                else if (hov) gfx_round_fill(&cc, x + 6, y + 1, iw - 18, ih - 2, 7, RGBA(255, 255, 255, 16));
                if (dt) gfx_round_rect(&cc, x + 6, y + 1, iw - 18, ih - 2, 7, UI_ACCENT);
                fileicon(&cc, e->kind, x + 16, y + (ih - 20) / 2, 20, e->name);
                int ty = y + (ih - font_ui.height) / 2;
                ui_text_fit(&ui, &font_ui, SIDE_W + 54, ty, lcol_size - SIDE_W - 62, e->name, UI_TEXT);
                char b[PATH_MAX];
                if (intrash) ui_text_fit(&ui, &font_ui, lcol_size, ty, lcol_mod - lcol_size - 10, e->orig ? e->orig : "?", UI_TEXT_DIM);
                else { if (e->dir) snprintf(b, sizeof b, "--"); else fmt_size(b, sizeof b, e->size); gfx_text(&cc, &font_ui, lcol_size, ty, b, UI_TEXT_DIM); }
                fmt_time(b, sizeof b, e->mtime);
                gfx_text(&cc, &font_ui, lcol_mod, ty, b, UI_TEXT_DIM);
            }
        }
        if (banding && (ui.buttons & 1)) {                               /* rubber band */
            int bx0 = band_x < mx - L.x ? band_x : mx - L.x, bx1 = band_x < mx - L.x ? mx - L.x : band_x;
            int cy = my - L.y + scroll * L.rh, by0 = band_y < cy ? band_y : cy, by1 = band_y < cy ? cy : band_y;
            int rx = L.x + bx0, ry = L.y + by0 - scroll * L.rh;
            gfx_fill(&cc, rx, ry, bx1 - bx0, by1 - by0, RGBA(0xB3, 0x88, 0xFF, 40));
            gfx_rect(&cc, rx, ry, bx1 - bx0, by1 - by0, RGBA(0xD8, 0xC4, 0xFF, 160));
        }
        ui_vscroll(&ui, W - 12, L.y, L.h, L.nrows, L.vis, &scroll);

        /* status bar */
        gfx_fill(c, 0, H - SB_H, W, SB_H, UI_PANEL);
        gfx_fill(c, 0, H - SB_H, W, 1, UI_BORDER);
        char st[200];
        int ns = nsel();
        if (ns) {
            uint64_t tot = 0;
            for (int j = 0; j < nents; j++) if (ents[j].sel && !ents[j].dir) tot += ents[j].size;
            char sz[32];
            fmt_size(sz, sizeof sz, tot);
            if (ns == 1) { int i0 = first_sel(); snprintf(st, sizeof st, "%d items   |   %s%s%s", nord, ents[i0].name, ents[i0].dir ? "" : "   ", ents[i0].dir ? "" : sz); }
            else snprintf(st, sizeof st, "%d items   |   %d selected   %s", nord, ns, sz);
        } else snprintf(st, sizeof st, nord == nents ? "%d items" : "%d matching items", nord);
        ui_text_fit(&ui, &font_ui, 14, H - SB_H + 6, W / 2, st, UI_TEXT_DIM);
        if (msg[0] && tick_ms < msg_until) {
            int mw = gfx_text_width(&font_ui, msg);
            gfx_text(c, &font_ui, W - 14 - mw, H - SB_H + 6, msg, msg_bad ? UI_BAD : UI_GOOD);
        }

        /* drag ghost */
        if (dragging) {
            int n = nsel();
            int gk = first_sel() >= 0 ? ents[first_sel()].kind : FK_FILE;
            fileicon(c, gk, mx - 14, my - 14, 28, first_sel() >= 0 ? ents[first_sel()].name : "");
            if (n > 1) {
                char b[8];
                snprintf(b, sizeof b, "%d", n);
                gfx_circle_fill(c, mx + 16, my - 14, 9, UI_ACCENT2);
                gfx_text_center(c, &font_ui_bold, mx + 7, my - 14 - font_ui_bold.height / 2, 18, b, RGB(0x1A, 0x10, 0x2E));
            }
            if (ctrl()) gfx_text(c, &font_ui_bold, mx + 24, my + 4, "copy", UI_GOOD);
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
                int mw = 210, mh = menu_h();
                int mxp = menu_x + mw > W ? W - mw - 4 : menu_x, myp = menu_y + mh > H ? H - mh - 4 : menu_y;
                gfx_shadow(c, mxp, myp, mw, mh, 10, 10, 120);
                gfx_round_fill(c, mxp, myp, mw, mh, 10, RGB(0x2A, 0x27, 0x3D));
                gfx_round_rect(c, mxp, myp, mw, mh, 10, RGBA(255, 255, 255, 45));
                int chosen = -1, y = myp + 6;
                for (int i = 0; i < nmenu; i++) {
                    if (menu[i].sep) { gfx_fill(c, mxp + 12, y + 4, mw - 24, 1, UI_BORDER); y += 9; continue; }
                    bool hov = !menu[i].dis && ui_hover(&ui, mxp + 6, y, mw - 12, 30);
                    if (hov) gfx_round_fill(c, mxp + 6, y, mw - 12, 30, 6, RGBA(0xB3, 0x88, 0xFF, 90));
                    gfx_text(c, &font_ui, mxp + 18, y + 6, menu[i].label, menu[i].dis ? RGBA(0x9A, 0x96, 0xB8, 120) : menu[i].id == A_TRASH || menu[i].id == A_DELETE || menu[i].id == A_EMPTY ? UI_BAD : UI_TEXT);
                    if (ui.pressed && hov) chosen = menu[i].id;
                    y += 30;
                }
                if (chosen >= 0 || (!fresh && (ui.pressed || ui.rpressed))) mode = M_NONE;
                if (chosen >= 0) do_action(chosen);
                while (ui_key(&ui, &k)) if (k.key == KEY_ESC) mode = M_NONE;
            } else if (mode == M_NEWDIR || mode == M_NEWFILE || mode == M_RENAME) {
                int r = ui_prompt(&ui, mode == M_NEWDIR ? "New folder" : mode == M_NEWFILE ? "New text file" : "Rename", mode == M_RENAME ? "Rename" : "Create", namebuf, sizeof namebuf);
                int m0 = mode;
                if (r) mode = M_NONE;
                if (r == 1) { if (m0 == M_RENAME) do_rename(namebuf); else do_new(namebuf, m0 == M_NEWDIR); }
            } else if (mode == M_DELETE || mode == M_EMPTY) {
                char t[160];
                int n = nsel();
                if (mode == M_EMPTY) snprintf(t, sizeof t, "Permanently delete all %d items in the Trash?", trash_n);
                else if (n == 1) snprintf(t, sizeof t, "Permanently delete \"%s\"%s?", ents[first_sel()].name, ents[first_sel()].dir ? " and everything in it" : "");
                else snprintf(t, sizeof t, "Permanently delete %d items?", n);
                int r = ui_confirm(&ui, mode == M_EMPTY ? "Empty Trash" : "Delete", t, "Delete");
                int m0 = mode;
                if (r) mode = M_NONE;
                if (r == 1) { if (m0 == M_EMPTY) { fs_trash_empty(); say(false, "%s", "Trash emptied"); refresh(false); } else delete_sel(); }
            } else if (mode == M_PROPS) {
                if (props_draw(&ui)) mode = M_NONE;
            }
        }
        bool more = thumb_make();
        ui_end(&ui);
        thumb_busy = more;
    } while (ui_poll(&ui, thumb_busy ? 0 : pend ? 60 : 1000));
    gui_window_destroy(w);
    return 0;
}
