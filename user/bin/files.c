/* files - file manager: icon grid + list views, sidebar, history, multi-select (click, Ctrl/Shift, rubber band),
 * clipboard, trash, drag and drop (within Files, to/from other windows and the desktop via DRAG_FILE), thumbnails.
 * Graphite layout (docs/DESIGN.md §9): toolbar with crumbs, Places sidebar, 96px grid / 28px table, status bar */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <syscall.h>
#include <fsutil.h>
#include <pthread.h>
#include "image.h"
#include "benchutil.h"

#define MAX_ENT 2048
#define HIST_MAX 32
#define TB_H H_TOOLBAR
#define SB_H H_STATUS
#define HDR_H H_ROW
#define LROW H_ROW
#define CW 96                         /* grid cell 96x96, 4px row gap */
#define CH 100
#define TH ICON_APP                   /* thumbnails fit the 48px icon box */
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
static int SW = W_SIDEBAR;            /* sidebar width; 0 below a 720px window */
static uint32_t dir_sig, tick_ms;
static char msg[128];
static bool msg_new, msg_undo;        /* msg_undo: the toast carries an Undo action */

enum { M_NONE, M_MENU, M_NEWDIR, M_NEWFILE, M_RENAME, M_DELETE, M_EMPTY, M_PROPS, M_CONFLICT };
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

static void say(bool bad, const char *fmt, const char *arg) {   /* shown as a toast */
    (void)bad;
    msg_undo = false;
    snprintf(msg, sizeof msg, fmt, arg);
    char *p = strstr(msg, "item(s)");                          /* "1 item", "2 items" */
    if (p) { bool one = !strcmp(arg, "1"); memmove(p + (one ? 4 : 5), p + 7, strlen(p + 7) + 1); if (!one) p[4] = 's'; }
    msg_new = true;
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
    if (image_load_fit(want, &im, TH) == 0) {          /* big JPEGs come back shrunk by 8 (block means): the thumbnail is a box average anyway */
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
    if (!d) say(true, "Cannot open “%s”", cwd);
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
    if (!strcmp(p, TRASH_FILES)) { mkdir(fs_trash_dir, 0755); mkdir(TRASH_FILES, 0755); }      /* nothing trashed yet */
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
        if (fs_open(p) < 0) { say(true, "No program opens “%s”", e->name); return; }
        snprintf(nm, sizeof nm, "%s", e->name);
        started++;
    }
    if (started) say(false, started == 1 ? "Opening “%s”" : "Opening “%s” and more", nm);
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

/* ---- undo: the last few Trash / Restore / rename / new-folder operations of this window (Ctrl+Z, or the toast's Undo) ---- */
enum { U_TRASH, U_RESTORE, U_RENAME, U_NEW };
struct uit { char a[PATH_MAX], b[PATH_MAX]; uint32_t mtime, size; };    /* trash: name, original; restore: path; rename: old, new */
struct undo { int kind, n; struct uit *it; };
#define NUNDO 8
static struct undo ustk[NUNDO];
static int nundo;

static void undo_push(int kind, struct uit *it, int n) {                 /* takes it; the toast offers Undo for this one */
    if (!it || !n) { free(it); return; }
    if (nundo == NUNDO) { free(ustk[0].it); memmove(ustk, ustk + 1, (NUNDO - 1) * sizeof *ustk); nundo--; }
    ustk[nundo++] = (struct undo){ kind, n, it };
    msg_undo = true;
}

static const char *items_s(int n) { static char b[4][24]; static int i; i = (i + 1) & 3; snprintf(b[i], sizeof b[i], n == 1 ? "%d item" : "%d items", n); return b[i]; }

/* ---- copy / move / delete run on a worker thread so the window stays alive. The worker touches no libgui state: it
 * only calls fsutil and updates the counters below, which the UI thread polls each frame (ponytail: plain volatile
 * words, a 32-bit byte counter (4 GiB) and a double-buffered name that can tear for one frame; add a mutex if they matter) ---- */
enum { J_COPY, J_MOVE, J_DELETE, J_EMPTY };
enum { C_NONE, C_REPLACE, C_SKIP, C_KEEP };
static struct job {
    int kind, n;
    char (*src)[PATH_MAX];
    char dir[PATH_MAX];
    uint32_t *it, *by;                                  /* per source: entries and bytes, from fs_du */
    pthread_t tid;
    bool active;                                        /* UI thread only: a job exists (running or waiting to be reaped) */
    uint32_t t0;
    volatile int done, ready, cancel, ask, answer, all_ans;   /* ask: a name conflict waits for `answer`; all_ans: remembered for the rest */
    volatile uint32_t items, items_total, bytes, bytes_total;
    char cur[2][96];
    volatile int ci;
    char ask_name[96];
    bool ask_dir;
    int ok, bad, skipped;
    char made[96];
} J;

static int job_hook(void *cx, const char *path, uint32_t b) {
    struct job *j = cx;
    if (b) j->bytes += b;
    else { int n = !j->ci; j->items++; snprintf(j->cur[n], sizeof j->cur[n], "%s", fs_base(path)); j->ci = n; }
    return j->cancel;
}

static int job_ask(struct job *j, const char *name, bool dir) {
    if (j->all_ans) return j->all_ans;
    snprintf(j->ask_name, sizeof j->ask_name, "%s", name);
    j->ask_dir = dir;
    j->answer = 0;
    j->ask = 1;
    while (!j->answer && !j->cancel) usleep(20000);
    j->ask = 0;
    return j->answer ? j->answer : C_SKIP;
}

static void job_one(struct job *j, int i) {
    const char *src = j->src[i];
    uint32_t it = j->it[i], by = j->by[i];
    char dst[PATH_MAX], tmp[PATH_MAX], par[PATH_MAX];
    int rc;
    if (j->kind == J_DELETE) {
        rc = fs_rmrf_x(src, job_hook, j);
        if (!rc && fs_in_trash(src)) { snprintf(tmp, sizeof tmp, "%s/info/%s", fs_trash_dir, fs_base(src)); unlink(tmp); }
        if (rc == FS_CANCEL) return;
        if (rc) j->bad++; else j->ok++;
        return;
    }
    bool move = j->kind == J_MOVE;
    snprintf(par, sizeof par, "%s", src);
    char *sl = strrchr(par, '/');
    if (sl && sl != par) *sl = 0; else if (sl) sl[1] = 0;
    size_t sn = strlen(src);
    if (!strncmp(j->dir, src, sn) && (j->dir[sn] == '/' || !j->dir[sn])) { j->bad++; return; }        /* into itself */
    bool same = !strcmp(par, j->dir);
    if (move && same) { j->items_total -= it; j->bytes_total -= by; return; }                           /* already there */
    join(dst, j->dir, fs_base(src));
    bool replace = false;
    if (exists(dst)) {
        if (same) fs_unique(dst, sizeof dst);                                                            /* a copy next to its original */
        else {
            int a = job_ask(j, fs_base(src), is_dir(dst));
            if (j->cancel) return;
            if (a == C_SKIP) { j->skipped++; j->items_total -= it; j->bytes_total -= by; return; }
            if (a == C_KEEP) fs_unique(dst, sizeof dst);
            else {
                size_t dl = strlen(dst);
                if (!strncmp(src, dst, dl) && src[dl] == '/') { j->bad++; return; }                      /* the source lives inside what it would replace */
                replace = true;
            }
        }
    }
    const char *to = dst;
    if (replace) { join(tmp, j->dir, ""); snprintf(tmp + strlen(tmp), sizeof tmp - strlen(tmp), ".%s.part", fs_base(src)); fs_unique(tmp, sizeof tmp); to = tmp; }
    if (j->cancel) return;
    if (move && rename(src, to) == 0) { j->items += it; j->bytes += by; rc = 0; }
    else {
        rc = fs_copy_x(src, to, job_hook, j);                                                            /* a cancelled copy removes itself */
        if (!rc && move) rc = fs_rmrf(src);                                                              /* past the point of no return: not cancellable */
    }
    if (rc == 0 && replace) rc = fs_rmrf(dst) || rename(tmp, dst) ? -1 : 0;                             /* old one goes only once the new one is whole */
    if (rc && replace) fs_rmrf(tmp);
    if (rc == FS_CANCEL) return;
    if (rc) { j->bad++; return; }
    j->ok++;
    snprintf(j->made, sizeof j->made, "%s", fs_base(dst));
}

static void *job_main(void *a) {
    struct job *j = a;
    char d[PATH_MAX];
    uint32_t f, dd;
    uint64_t b;
    if (j->kind == J_EMPTY) {
        snprintf(d, sizeof d, "%s/files", fs_trash_dir);
        fs_du(d, &f, &dd, &b);
        j->items_total = f + dd ? f + dd - 1 : 0;                                                         /* the files folder itself stays */
    } else for (int i = 0; i < j->n; i++) {
        fs_du(j->src[i], &f, &dd, &b);
        j->it[i] = f + dd; j->by[i] = b > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)b;
        j->items_total += j->it[i]; j->bytes_total += j->by[i];
    }
    j->ready = 1;
    if (j->kind == J_EMPTY) {
        int rc = fs_trash_empty_x(job_hook, j);
        if (rc == 0) j->ok = 1; else if (rc != FS_CANCEL) j->bad = 1;
    } else for (int i = 0; i < j->n && !j->cancel; i++) job_one(j, i);
    j->done = 1;
    return NULL;
}

/* queue a job over n paths (copied); dir is the destination of a copy / move */
static void job_start(int kind, char (*ps)[PATH_MAX], int n, const char *dir) {
    if (J.active) { say(true, "%s", "Another file operation is still running"); return; }
    memset(&J, 0, sizeof J);
    J.kind = kind; J.n = n;
    J.src = malloc((n + 1) * PATH_MAX);
    J.it = calloc(n + 1, sizeof *J.it);
    J.by = calloc(n + 1, sizeof *J.by);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);                                                           /* the fs walks recurse */
    if (!J.src || !J.it || !J.by) goto fail;
    if (n) memcpy(J.src, ps, (size_t)n * PATH_MAX);
    snprintf(J.dir, sizeof J.dir, "%s", dir ? dir : "");
    J.t0 = uptime_ms();
    if (pthread_create(&J.tid, &at, job_main, &J)) goto fail;
    J.active = true;
    return;
fail:
    free(J.src); free(J.it); free(J.by);
    memset(&J, 0, sizeof J);
    say(true, "%s", "Could not start the operation");
}

static void job_finish(void) {                                                                           /* the worker is done: report */
    pthread_join(J.tid, NULL);
    static const char *const past[] = { "Copied", "Moved", "Deleted", "Emptied" }, *const verb[] = { "copied", "moved", "deleted", "emptied" };
    char b[160];
    int k = J.kind, bad = J.bad, ok = J.ok;
    b[0] = 0;
    if (J.cancel) { if (ok) snprintf(b, sizeof b, "Cancelled · %s %s", items_s(ok), verb[k]); else snprintf(b, sizeof b, "Cancelled · nothing %s", verb[k]); }
    else if (bad) { snprintf(b, sizeof b, "%s could not be %s", items_s(bad), verb[k]); if (ok) snprintf(b + strlen(b), sizeof b - strlen(b), " · %d done", ok); }
    else if (k == J_EMPTY) snprintf(b, sizeof b, "Emptied the Trash");
    else if (ok) { snprintf(b, sizeof b, "%s %s", past[k], items_s(ok)); if (J.skipped) snprintf(b + strlen(b), sizeof b - strlen(b), " · skipped %d", J.skipped); }
    else if (J.skipped) snprintf(b, sizeof b, "Skipped %s", items_s(J.skipped));
    if (b[0]) say(bad != 0, "%s", b);
    char made[96];
    snprintf(made, sizeof made, "%s", J.made);
    bool pick = ok == 1 && k <= J_MOVE && !strcmp(J.dir, cwd);
    free(J.src); free(J.it); free(J.by);
    J.active = false;
    refresh(false);
    if (pick) select_name(made);
}

static void paste_list(char (*ps)[PATH_MAX], int n, const char *dir, bool copy) { job_start(copy ? J_COPY : J_MOVE, ps, n, dir); }

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

static char (*collect_all(int *n))[PATH_MAX] {                  /* paths of the whole selection */
    char (*ps)[PATH_MAX] = malloc((nsel() + 1) * PATH_MAX);
    *n = ps ? collect(ps, nsel()) : 0;
    return ps;
}

static void trash_sel(void) {
    int n, ok = 0;
    char (*ps)[PATH_MAX] = collect_all(&n);
    struct uit *u = calloc(n + 1, sizeof *u);
    for (int i = 0; ps && u && i < n; i++) {
        if (protected_path(ps[i])) { say(true, "“%s” is protected", fs_base(ps[i])); continue; }
        if (fs_trash_put(ps[i], u[ok].a, sizeof u[ok].a) == 0) { fs_trash_info(u[ok].a, u[ok].b, sizeof u[ok].b, NULL); ok++; }
    }
    char t[16];
    snprintf(t, sizeof t, "%d", ok);
    if (ok) { say(false, "Moved %s item(s) to Trash", t); undo_push(U_TRASH, u, ok); u = NULL; }
    else if (n) say(true, "%s", "Could not move to Trash");
    free(u); free(ps);
    int keep = cur;
    refresh(false);
    if (keep >= 0 && nord) anchor = cur = keep < nord ? keep : nord - 1;
}

static void delete_sel(void) {                          /* permanent: a job, with progress */
    int n, m = 0;
    char (*ps)[PATH_MAX] = collect_all(&n);
    for (int i = 0; ps && i < n; i++) {
        if (protected_path(ps[i])) { say(true, "“%s” is protected", fs_base(ps[i])); continue; }
        if (m != i) memcpy(ps[m], ps[i], PATH_MAX);
        m++;
    }
    if (m) job_start(J_DELETE, ps, m, NULL);
    free(ps);
}

static void restore_sel(void) {
    int ok = 0, n;
    char t[16], dest[PATH_MAX];
    char (*ps)[PATH_MAX] = collect_all(&n);                      /* the selected names, as paths (only the base is used) */
    struct uit *u = calloc(n + 1, sizeof *u);
    for (int i = 0; ps && u && i < n; i++) {
        if (fs_trash_restore(fs_base(ps[i]), dest, sizeof dest)) continue;
        struct stat st;
        snprintf(u[ok].a, sizeof u[ok].a, "%s", dest);
        if (stat(dest, &st) == 0 && !S_ISDIR(st.st_mode)) { u[ok].mtime = st.st_mtime; u[ok].size = st.st_size; }
        ok++;
    }
    snprintf(t, sizeof t, "%d", ok);
    if (ok) { say(false, "Restored %s item(s)", t); undo_push(U_RESTORE, u, ok); u = NULL; } else say(true, "%s", "Could not restore");
    free(u); free(ps);
    refresh(false);
}

static void do_new(const char *name, bool dir) {
    if (!name[0] || strchr(name, '/')) { say(true, "%s", "Invalid name"); return; }
    char p[PATH_MAX];
    join(p, cwd, name);
    if (exists(p)) { say(true, "“%s” already exists", name); return; }
    bool ok;
    if (dir) ok = mkdir(p, 0755) == 0;
    else { int fd = open(p, O_WRONLY | O_CREAT, 0644); ok = fd >= 0; if (ok) close(fd); }
    if (!ok) { say(true, "Could not create “%s”", name); return; }
    say(false, "Created “%s”", name);
    struct uit *u = calloc(1, sizeof *u);
    if (u) { snprintf(u->a, sizeof u->a, "%s", p); undo_push(U_NEW, u, 1); }
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
    if (exists(b)) { say(true, "“%s” already exists", name); return; }
    if (rename(a, b)) { say(true, "Could not rename to “%s”", name); return; }
    say(false, "Renamed to “%s”", name);
    struct uit *u = calloc(1, sizeof *u);
    if (u) { snprintf(u->a, sizeof u->a, "%s", a); snprintf(u->b, sizeof u->b, "%s", b); undo_push(U_RENAME, u, 1); }
    refresh(false);
    select_name(name);
}

/* undo the newest operation. Nothing is overwritten or deleted that changed since: such items are skipped and the toast says why */
static void do_undo(void) {
    if (!nundo) { say(true, "%s", "Nothing to undo"); return; }
    struct undo u = ustk[--nundo];
    int done = 0, skip = 0;
    const char *why = "";
    char what[96] = "";
    char b[PATH_MAX], dest[PATH_MAX], org[PATH_MAX];
    struct stat st;
    for (int i = 0; i < u.n; i++) {
        struct uit *t = &u.it[i];
        bool ok = false;
        switch (u.kind) {
        case U_TRASH:
            snprintf(b, sizeof b, "%s/files/%s", fs_trash_dir, t->a);
            if (!exists(b)) why = "no longer in the Trash";
            else if (exists(t->b)) why = "the original name is taken";
            else if (fs_trash_info(t->a, org, sizeof org, NULL) || strcmp(org, t->b) || fs_trash_restore(t->a, dest, sizeof dest)) why = "it can't be put back";
            else ok = true;
            break;
        case U_RESTORE:
            if (stat(t->a, &st)) why = "it was moved or deleted";
            else if (!S_ISDIR(st.st_mode) && ((uint32_t)st.st_mtime != t->mtime || (uint32_t)st.st_size != t->size)) why = "it changed since";
            else if (fs_trash_put(t->a, NULL, 0)) why = "it can't be moved to the Trash";
            else ok = true;
            break;
        case U_RENAME:
            if (!exists(t->b)) why = "it was moved or deleted";
            else if (exists(t->a)) why = "the old name is taken";
            else if (rename(t->b, t->a)) why = "it can't be renamed";
            else ok = true;
            break;
        case U_NEW:
            if (stat(t->a, &st)) why = "it was moved or deleted";
            else if (S_ISDIR(st.st_mode) ? rmdir(t->a) : (st.st_size ? 1 : unlink(t->a))) why = "it is no longer empty";
            else ok = true;
            break;
        }
        if (ok) { done++; if (!what[0]) snprintf(what, sizeof what, "%s", fs_base(u.kind == U_TRASH ? t->b : t->a)); } else skip++;
    }
    static const char *const did[] = { "Put back %s", "Moved %s back to the Trash", "Renamed back to “%s”", "Removed “%s”" };
    char head[96], m[160];
    if (u.kind == U_RENAME || u.kind == U_NEW) snprintf(head, sizeof head, did[u.kind], what);
    else if (u.kind == U_TRASH && done == 1) snprintf(head, sizeof head, "Put back “%s”", what);
    else snprintf(head, sizeof head, did[u.kind], items_s(done));
    if (!skip) snprintf(m, sizeof m, "%s", head);
    else if (!done) snprintf(m, sizeof m, "Can't undo: %s", why);
    else snprintf(m, sizeof m, "%s · skipped %d (%s)", head, skip, why);
    say(skip != 0, "%s", m);
    free(u.it);
    refresh(false);
    if (done == 1 && what[0] && (u.kind == U_TRASH || u.kind == U_RENAME)) select_name(what);
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
    SW = W < 720 ? 0 : W_SIDEBAR;
    L.w = W - SW;
    if (vgrid) {                                   /* 96px cells, 12 16 padding */
        L.x = SW + SP_5; L.y = TB_H + SP_4; L.rh = CH;
        L.cols = (L.w - 2 * SP_5) / CW;
        if (L.cols < 1) L.cols = 1;
    } else { L.x = SW; L.y = TB_H + HDR_H; L.rh = LROW; L.cols = 1; }
    L.h = H - SB_H - L.y;
    L.vis = L.h / L.rh;
    if (L.vis < 1) L.vis = 1;
    L.nrows = (nord + L.cols - 1) / L.cols;
    if (scroll > L.nrows - L.vis) scroll = L.nrows - L.vis;
    if (scroll < 0) scroll = 0;
}

static void item_rect(int k, int *x, int *y, int *w, int *h) {
    int r = k / L.cols, c = k % L.cols;
    *x = vgrid ? L.x + c * CW : SW; *y = L.y + (r - scroll) * L.rh;
    *w = vgrid ? CW : L.w; *h = vgrid ? CW : L.rh;
}

static int item_at(int px, int py, int W) {
    if (py < L.y || py >= L.y + L.h || px < SW || px >= W - 12) return -1;
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

/* the FILEICON_* for a path: special folders get their emblem */
static int ficon(const char *path, bool dir, int kind) {
    if (dir) {
        static const struct { const char *p; int id; } sp[] = {
            { "/home", FILEICON_FOLDER_HOME }, { "/home/Desktop", FILEICON_FOLDER_DESKTOP }, { "/home/Documents", FILEICON_FOLDER_DOCUMENTS },
            { "/home/Downloads", FILEICON_FOLDER_DOWNLOADS }, { "/home/Music", FILEICON_FOLDER_MUSIC }, { "/home/Pictures", FILEICON_FOLDER_PICTURES },
            { "/home/projects", FILEICON_FOLDER_PROJECTS }, { "/home/.trash", FILEICON_FOLDER_TRASH }, { TRASH_FILES, FILEICON_FOLDER_TRASH },
        };
        for (unsigned i = 0; i < sizeof sp / sizeof sp[0]; i++) if (!strcmp(path, sp[i].p)) return sp[i].id;
        return FILEICON_FOLDER;
    }
    const char *x = strrchr(fs_base(path), '.');
    x = x && x != fs_base(path) ? x + 1 : "";
    switch (kind) {
    case FK_IMAGE: return FILEICON_IMAGE;
    case FK_AUDIO: return FILEICON_AUDIO;
    case FK_HTML: return FILEICON_WEB;
    case FK_TEXT: return FILEICON_TEXT;
    case FK_CODE: return strcasecmp(x, "sh") ? FILEICON_CODE : FILEICON_SCRIPT;
    case FK_EXE: return FILEICON_EXEC;
    }
    static const char *const arc[] = { "tar", "gz", "tgz", "zip", "bz2", "xz", "7z" };
    for (unsigned i = 0; i < sizeof arc / sizeof arc[0]; i++) if (!strcasecmp(x, arc[i])) return FILEICON_ARCHIVE;
    return FILEICON_GENERIC;
}

/* ---- sidebar: group labels have no path ---- */
static const struct place { const char *label, *path; int ico; } places[] = {
    { "Places", 0, 0 }, { "Home", "/home", ICON_HOME }, { "Desktop", "/home/Desktop", ICON_MONITOR },
    { "Documents", "/home/Documents", ICON_FILE_TEXT }, { "Pictures", "/home/Pictures", ICON_IMAGE }, { "Music", "/home/Music", ICON_MUSIC },
    { "Projects", "/home/projects", ICON_FOLDER_CODE }, { "Trash", TRASH_FILES, ICON_TRASH },
    { "System", 0, 0 }, { "Root", "/", ICON_DRIVE }, { "Programs", "/bin", ICON_TERMINAL }, { "Config", "/etc", ICON_SETTINGS },
};
#define NPLACES ((int)(sizeof places / sizeof places[0]))
static int place_y(int i) {                     /* labels: 8 above the first, 16 above the rest, 4 below */
    int y = TB_H;
    for (int j = 0; j < i; j++) y += places[j].path ? H_ROW : (j ? SP_5 : SP_3) + T_CAPTION_B->height + SP_2;
    return places[i].path ? y : y + (i ? SP_5 : SP_3);
}
static int place_at(int x, int y) {
    if (!SW) return -1;
    for (int i = 0; i < NPLACES; i++) if (places[i].path && x >= SP_3 && x < SW - SP_3 && y >= place_y(i) && y < place_y(i) + H_ROW) return i;
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

/* ---- properties ---- */
static struct {
    char name[96], type[40], where[PATH_MAX], size[64], when[40], perm[16], prog[96], orig[PATH_MAX];
    int icon, count;
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
        pr.icon = ficon(cwd, true, FK_DIR); pr.count = 1;
    } else if (n == 1) {
        struct ent *e = &ents[one];
        static const char *tn[] = { "Folder", "Program", "Text document", "Source code", "Image", "Audio", "Web page", "File" };
        snprintf(pr.name, sizeof pr.name, "%s", e->name);
        snprintf(pr.type, sizeof pr.type, "%s", tn[e->kind]);
        snprintf(pr.where, sizeof pr.where, "%s", cwd);
        join(p, cwd, e->name);
        pr.icon = ficon(p, e->dir, e->kind); pr.count = 1;
        struct stat st;
        if (stat(p, &st) == 0) {
            int m = st.st_mode;
            for (int i = 0; i < 9; i++) pr.perm[i] = m & (0400 >> i) ? "rwxrwxrwx"[i] : '-';
        }
        if (!e->dir) {
            char prog[PATH_MAX];
            int r = fs_assoc(p, prog, sizeof prog);
            snprintf(pr.prog, sizeof pr.prog, "%s", r == 1 ? fs_base(prog) : r == 2 ? "Runs itself" : "None");
        }
        if (e->orig) snprintf(pr.orig, sizeof pr.orig, "%s", e->orig);
    } else {
        snprintf(pr.name, sizeof pr.name, "%d items", n);
        snprintf(pr.type, sizeof pr.type, "Selection");
        snprintf(pr.where, sizeof pr.where, "%s", cwd);
        pr.icon = FILEICON_GENERIC; pr.count = n;
    }
    char sz[32];
    fmt_size(sz, sizeof sz, bytes);
    if (dirs || n > 1 || n == 0) snprintf(pr.size, sizeof pr.size, "%s (%u files, %u folders)", sz, (unsigned)files, (unsigned)dirs);
    else snprintf(pr.size, sizeof pr.size, "%s", sz);
    if (n <= 1) fmt_time(pr.when, sizeof pr.when, mt);
    mode = M_PROPS;
}

/* a dialog surface over the scrim (docs/DESIGN.md §9 Dialogs) */
static void dialog_box(struct ui *ui, int pw, int ph, int *ox, int *oy) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h, px = (W - pw) / 2, py = (H - ph) / 2;
    gfx_fill(c, 0, 0, W, H, ui_th.scrim);
    gfx_elev(c, px, py, pw, ph, R_3, E4, ui_th.elev[E4]);
    gfx_round_fill(c, px, py, pw, ph, R_3, ui_th.overlay);
    gfx_round_rect(c, px, py, pw, ph, R_3, UI_LINE);
    *ox = px; *oy = py;
}

static int props_draw(struct ui *ui) {
    const char *lab[] = { "Location", "Size", "Modified", "Permissions", "Opens with", "Original" };
    const char *val[] = { pr.where, pr.size, pr.when, pr.perm, pr.prog, pr.orig };
    int nr = 0;
    for (int i = 0; i < 6; i++) nr += val[i][0] != 0;
    int W = ui->win->w, pw = W - 2 * SP_5 < W_FORM ? W - 2 * SP_5 : W_FORM;
    int ph = SP_6 + ICON_APP + SP_5 + 1 + SP_3 + nr * H_ROW + SP_6 + H_CTL + SP_6, px, py;
    dialog_box(ui, pw, ph, &px, &py);
    struct canvas *c = ui->c;
    ui_aicon(ui, px + SP_6, py + SP_6, fileicon_get(pr.icon, ICON_APP));
    int tx = px + SP_6 + ICON_APP + SP_5, tw = pw - (tx - px) - SP_6, ty = py + SP_6 + (ICON_APP - T_HEADING->height - SP_1 - T_BODY->height) / 2;
    ui_text_fit(ui, T_HEADING, tx, ty, tw, pr.name, UI_TEXT);
    ui_text_fit(ui, T_BODY, tx, ty + T_HEADING->height + SP_1, tw, pr.type, UI_TEXT2);
    int y = py + SP_6 + ICON_APP + SP_5;
    gfx_hline(c, px + SP_6, y, pw - 2 * SP_6, UI_LINE);
    y += 1 + SP_3;
    for (int i = 0; i < 6; i++) {
        if (!val[i][0]) continue;
        int ry = y + (H_ROW - T_BODY->height) / 2;
        gfx_text(c, T_BODY, px + SP_6, ry, lab[i], UI_TEXT2);
        ui_text_fit(ui, T_BODY, px + SP_6 + 112, ry, pw - 2 * SP_6 - 112, val[i], UI_TEXT);
        y += H_ROW;
    }
    int r = 0, bw = ui_button_w(UI_NOICON, "Close") < W_CTL_MIN ? W_CTL_MIN : ui_button_w(UI_NOICON, "Close");
    if (ui_button(ui, px + pw - SP_6 - bw, py + ph - SP_6 - H_CTL, bw, H_CTL, "Close", UI_PRIMARY)) r = 1;
    struct gui_event k;
    while (ui_key(ui, &k)) if (k.key == KEY_ESC || k.key == KEY_ENTER) r = 1;
    return r;
}

/* the name conflict dialog (Replace / Skip / Keep both, plus "apply to all" when more items are queued) */
static bool cf_first, cf_all;
static int conflict_draw(struct ui *ui) {
    char title[PATH_MAX], body[200];
    snprintf(title, sizeof title, "“%s” already exists", J.ask_name);
    snprintf(body, sizeof body, "“%s” already has %s with this name. Replace it, keep both (the new one gets a number), or skip this one.%s",
             fs_base(J.dir), J.ask_dir ? "a folder" : "an item", J.ask_dir ? " Replacing a folder deletes everything in it." : "");
    int W = ui->win->w, pw = W - 2 * SP_5 < W_ALERT ? W - 2 * SP_5 : W_ALERT, tw = pw - 2 * SP_6, px, py;
    bool multi = J.n > 1;
    int bh = ui_text_wrap(ui, T_BODY, 0, 0, tw, body, 0);
    int ph = SP_6 + T_HEADING->height + SP_3 + bh + (multi ? SP_4 + SWITCH_H : 0) + SP_6 + H_CTL + SP_6;
    dialog_box(ui, pw, ph, &px, &py);
    ui_text_fit(ui, T_HEADING, px + SP_6, py + SP_6, tw, title, UI_TEXT);
    ui_text_wrap(ui, T_BODY, px + SP_6, py + SP_6 + T_HEADING->height + SP_3, tw, body, UI_TEXT2);
    if (multi) {
        int sy = py + SP_6 + T_HEADING->height + SP_3 + bh + SP_4;
        ui_switch(ui, px + SP_6, sy, &cf_all, 0);
        gfx_text(ui->c, T_BODY, px + SP_6 + SWITCH_W + SP_3, sy + (SWITCH_H - T_BODY->height) / 2, "Do this for all remaining conflicts", UI_TEXT);
    }
    static const char *const lab[] = { "Skip", "Replace", "Keep both" };       /* the primary sits rightmost */
    static const int ans[] = { C_SKIP, C_REPLACE, C_KEEP };
    int r = 0, by = py + ph - SP_6 - H_CTL, x = px + pw - SP_6;
    for (int i = 2; i >= 0; i--) {
        int bw = ui_button_w(UI_NOICON, lab[i]) < W_CTL_MIN ? W_CTL_MIN : ui_button_w(UI_NOICON, lab[i]);
        x -= bw;
        if (cf_first && i == 2) ui->focus = ui_id(x, by, bw, H_CTL);
        if (ui_button(ui, x, by, bw, H_CTL, lab[i], i == 2 ? UI_PRIMARY : i == 1 ? UI_DANGER : UI_SECONDARY)) r = ans[i];
        x -= SP_3;
    }
    cf_first = false;
    struct gui_event k;
    while (ui_key(ui, &k)) { if (k.key == KEY_ESC) r = C_SKIP; else if (k.key == KEY_ENTER) r = C_KEEP; }
    return r;
}

/* the progress card: bottom right, above the status bar, once a job has run for a moment */
static void job_card(struct ui *ui, int W, int H) {
    if (!J.active || J.done || mode == M_CONFLICT || uptime_ms() - J.t0 < 300) return;
    static const char *const ttl[] = { "Copying", "Moving", "Deleting", "Emptying the Trash" };
    char t[PATH_MAX + 64], st[96], name[96];
    int total = J.items_total, done = J.items < J.items_total ? J.items : J.items_total;
    if (J.kind == J_EMPTY) snprintf(t, sizeof t, "%s", ttl[J.kind]);
    else if (J.kind == J_DELETE) snprintf(t, sizeof t, "%s %s", ttl[J.kind], items_s(J.n));
    else snprintf(t, sizeof t, "%s %s to “%s”", ttl[J.kind], items_s(J.n), fs_base(J.dir));
    snprintf(name, sizeof name, "%s", J.cur[J.ci]);
    if (!J.ready) snprintf(st, sizeof st, "Preparing…");
    else {
        snprintf(st, sizeof st, "%d of %s", done, items_s(total));
        if (J.bytes_total && J.kind <= J_MOVE) {
            char a[24], b[24];
            fmt_size(a, sizeof a, J.bytes < J.bytes_total ? J.bytes : J.bytes_total);
            fmt_size(b, sizeof b, J.bytes_total);
            snprintf(st + strlen(st), sizeof st - strlen(st), " · %s of %s", a, b);
        }
    }
    int cw = W - 2 * SP_4 < 360 ? W - 2 * SP_4 : 360, iw = cw - 2 * SP_4;
    int ch = SP_4 + T_BODY_B->height + SP_1 + T_CAPTION->height + SP_3 + 4 + SP_3 + H_CTL + SP_4;
    int x = W - cw - SP_4, y = H - SB_H - SP_4 - ch;
    struct canvas *c = ui->c;
    gfx_elev(c, x, y, cw, ch, R_3, E2, ui_th.elev[E2]);
    gfx_round_fill(c, x, y, cw, ch, R_3, ui_th.overlay);
    gfx_round_rect(c, x, y, cw, ch, R_3, UI_LINE);
    ui_text_fit(ui, T_BODY_B, x + SP_4, y + SP_4, iw, t, UI_TEXT);
    ui_text_fit(ui, T_CAPTION, x + SP_4, y + SP_4 + T_BODY_B->height + SP_1, iw, name, UI_TEXT2);
    int by = y + SP_4 + T_BODY_B->height + SP_1 + T_CAPTION->height + SP_3;
    if (!J.ready) ui_progress(ui, x + SP_4, by, iw, -1, 0);
    else if (J.bytes_total && J.kind <= J_MOVE) ui_progress(ui, x + SP_4, by, iw, J.bytes >> 10, J.bytes_total >> 10);
    else ui_progress(ui, x + SP_4, by, iw, done, total);
    int bw = ui_button_w(UI_NOICON, "Cancel") < W_CTL_MIN ? W_CTL_MIN : ui_button_w(UI_NOICON, "Cancel");
    int ry = by + 4 + SP_3;
    gfx_text(c, T_CAPTION, x + SP_4, ry + (H_CTL - T_CAPTION->height) / 2, st, UI_TEXT2);
    if (ui_button(ui, x + cw - SP_4 - bw, ry, bw, H_CTL, "Cancel", UI_SECONDARY)) J.cancel = 1;
}

/* ---- context menu (unavailable items are hidden) ---- */
static void menu_add(const char *label, int id, bool dis) { if (!dis) menu[nmenu++] = (struct mi){ label, id, false, false }; }
static void menu_sep(void) { if (nmenu && !menu[nmenu - 1].sep) menu[nmenu++] = (struct mi){ 0, 0, true, false }; }

static void menu_open_at(int x, int y, int what) {             /* what: 0 background, 1 item(s), 2 trash place */
    nmenu = 0;
    int n = nsel();
    if (what == 2) { menu_add("Open", A_GOTRASH, false); menu_add("Empty Trash…", A_EMPTY, trash_n == 0); }
    else if (what == 1) {
        menu_add("Open", A_OPEN, false);
        if (n == 1 && first_sel() >= 0 && !ents[first_sel()].dir) menu_add("Open with Text Editor", A_OPENTXT, false);
        menu_sep();
        if (intrash) { menu_add("Restore", A_RESTORE, false); menu_add("Delete permanently…", A_DELETE, false); }
        else {
            menu_add("Cut", A_CUT, false); menu_add("Copy", A_COPY, false);
            menu_sep();
            menu_add("Rename…", A_RENAME, n != 1);
            menu_add("Move to Trash", A_TRASH, false);
        }
        menu_sep();
        menu_add("Properties", A_PROPS, false);
    } else {
        if (intrash) { menu_add("Empty Trash…", A_EMPTY, nents == 0); menu_sep(); }
        else {
            menu_add("New folder…", A_NEWDIR, false); menu_add("New text file…", A_NEWFILE, false);
            menu_add("Paste", A_PASTE, false);
            menu_sep();
        }
        menu_add("Select all", A_SELALL, false);
        menu_add(vgrid ? "View as list" : "View as icons", A_VIEW, false);
        menu_add("Sort by name", A_SORTN, false); menu_add("Sort by size", A_SORTS, false); menu_add("Sort by date", A_SORTM, false);
        menu_add(hidden ? "Hide hidden files" : "Show hidden files", A_HIDDEN, false);
        menu_add("Refresh", A_REFRESH, false);
        menu_sep();
        menu_add("Properties", A_PROPS, false);
    }
    if (nmenu && menu[nmenu - 1].sep) nmenu--;
    menu_x = x; menu_y = y;
    mode = M_MENU;
}

static int menu_w(void) {
    int w = W_MENU_MIN;
    for (int i = 0; i < nmenu; i++) if (!menu[i].sep && gfx_text_width(T_BODY, menu[i].label) + 2 * SP_4 + 2 * SP_2 > w) w = gfx_text_width(T_BODY, menu[i].label) + 2 * SP_4 + 2 * SP_2;
    return w < W_MENU_MAX ? w : W_MENU_MAX;
}
static int menu_h(void) { int h = 2 * SP_2; for (int i = 0; i < nmenu; i++) h += menu[i].sep ? 2 * SP_2 + 1 : H_ROW; return h; }

/* draws the open menu; returns the action picked or -1 */
static int menu_draw(struct ui *ui) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h, mw = menu_w(), mh = menu_h();
    int x = menu_x + mw > W - SP_2 ? menu_x - mw : menu_x, y = menu_y + mh > H - SP_2 ? menu_y - mh : menu_y;   /* flip left / up */
    if (x < SP_2) x = SP_2;
    if (y < SP_2) y = SP_2;
    gfx_elev(c, x, y, mw, mh, R_3, E2, ui_th.elev[E2]);
    gfx_round_fill(c, x, y, mw, mh, R_3, ui_th.overlay);
    gfx_round_rect(c, x, y, mw, mh, R_3, UI_LINE);
    int chosen = -1, ry = y + SP_2;
    for (int i = 0; i < nmenu; i++) {
        if (menu[i].sep) { gfx_hline(c, x + SP_4, ry + SP_2, mw - 2 * SP_4, UI_LINE); ry += 2 * SP_2 + 1; continue; }
        bool hov = ui_hover(ui, x + SP_2, ry, mw - 2 * SP_2, H_ROW);
        if (hov) gfx_round_fill(c, x + SP_2, ry, mw - 2 * SP_2, H_ROW, R_2, ui->buttons & 1 ? ui_th.press_ov : ui_th.hover_ov);
        int id = menu[i].id;
        ui_text_fit(ui, T_BODY, x + SP_2 + SP_4, ry + (H_ROW - T_BODY->height) / 2, mw - 2 * (SP_2 + SP_4), menu[i].label,
                    id == A_DELETE || id == A_EMPTY ? UI_BAD : UI_TEXT);
        if (ui->pressed && hov) chosen = id;
        ry += H_ROW;
    }
    return chosen;
}

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
    int n;
    char (*ps)[PATH_MAX] = collect_all(&n);
    if (!ps) return;
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

/* breadcrumbs for cwd ("Root" first), leading parts dropped until they fit; navigates on a click.
 * Returns the x where the crumbs end (a click past it edits the path) */
static int crumbs(struct ui *ui, int x, int y, int maxw) {
    const char *parts[64];
    char buf[PATH_MAX];
    if (intrash) { static const char *const t[] = { "Trash" }; ui_crumbs(ui, x, y, maxw, t, 1); return x + gfx_text_width(T_BODY_B, t[0]) + 2 * SP_3; }
    int ends[64], n = 0;
    snprintf(buf, sizeof buf, "%s", cwd);
    parts[n] = "Root"; ends[n++] = 1;
    for (char *s = buf + 1; *s && n < 64; ) {
        char *e = strchr(s, '/');
        parts[n] = s; ends[n++] = (e ? e : s + strlen(s)) - buf;
        if (!e) break;
        *e = 0; s = e + 1;
    }
    int ws[64], tot = 0, first = 0;
    for (int i = 0; i < n; i++) { ws[i] = gfx_text_width(i == n - 1 ? T_BODY_B : T_BODY, parts[i]) + 2 * SP_3 + (i < n - 1 ? ICON_SM : 0); tot += ws[i]; }
    while (first < n - 1 && tot > maxw) tot -= ws[first++];
    int hit = ui_crumbs(ui, x, y, maxw, parts + first, n - first);
    if (hit >= 0) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%.*s", ends[first + hit], cwd);
        go(p, true);
    }
    return x + (tot < maxw ? tot : maxw);
}

/* `files --bench DIR`: list DIR, then make every image's thumbnail; spans go to /dev/kmsg */
static void bench(const char *dir) {
    uint64_t t = tsc();
    go(dir, true);
    blog("FILES %s list %u us, %d entries", dir, us(tsc() - t), nents);
    t = tsc();
    uint32_t worst = 0; int n = 0;
    for (int i = 0; i < nents && n < NTHUMB; i++) {
        if (ents[i].kind != FK_IMAGE) continue;
        char p[PATH_MAX]; join(p, cwd, ents[i].name);
        thumb_get(p, ents[i].mtime, ents[i].size);
        uint64_t t1 = tsc(); thumb_make(); uint32_t d = us(tsc() - t1); if (d > worst) worst = d; n++;
    }
    blog("FILES %s thumbnails %d in %u us (worst %u us)", dir, n, us(tsc() - t), worst);
    t = tsc(); for (int i = 0; i < 20; i++) sig_of(cwd); blog("FILES %s sig_of %u us", dir, us(tsc() - t) / 20);
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--bench")) { bench(argv[2]); return 0; }
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
    char title[128] = "";
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
        if (J.active && J.done) job_finish();
        if (ui_toast_action(&ui)) do_undo();
        if (J.active && J.ask && !J.answer && (mode == M_NONE || mode == M_MENU)) { mode = M_CONFLICT; cf_first = true; cf_all = false; }
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

        /* toolbar geometry: back/forward/up, crumbs (or the path entry), search, view switch, more */
        int ty = (TB_H - H_CTL) / 2, cx0 = SP_3 + 3 * (H_CTL + SP_2) - SP_2 + SP_4;
        int more_x = W - SP_3 - H_CTL, seg_x = more_x - SP_4 - (3 * SP_1 + 2 * H_CTL);
        int srch_w = (W - cx0) / 3 > 240 ? 240 : (W - cx0) / 3 < 120 ? 120 : (W - cx0) / 3, srch_x = seg_x - SP_4 - srch_w;
        int srch_y = (TB_H - H_SEARCH) / 2, cmaxw = srch_x - SP_4 - cx0;
        uint32_t path_id = ui_id(cx0, ty, cmaxw, H_CTL), search_id = ui_id(srch_x, srch_y, srch_w, H_SEARCH);
        bool typing = ui.focus == path_id || ui.focus == search_id;
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
                    case 'z': do_undo(); break;
                    case 'n': do_action(A_NEWDIR); break;
                    case 'h': do_action(A_HIDDEN); break;
                    case 'l': ui.focus = path_id; path_fresh = true; snprintf(pathbox, sizeof pathbox, "%s", cwd); break;
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
        } else if (ui.focus == path_id) {
            for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i].key == KEY_ESC) { ui.focus = 0; ui.nkeys = 0; break; }
            if (path_fresh)                                                /* typing over a freshly focused path replaces it */
                for (int i = 0; i < ui.nkeys; i++) if (ui.keys[i].ch >= 32 && ui.keys[i].ch < 127 && !(ui.keys[i].mods & MOD_CTRL)) { pathbox[0] = 0; path_fresh = false; break; }
        }
        layout(W, H);

        /* ---- mouse in the content area ---- */
        int mx = ui.mx, my = ui.my;
        bool in_c = mx >= SW && mx < W - 12 && my >= TB_H && my < H - SB_H;
        if (!modal && ui_hover(&ui, SW, L.y, L.w, L.h) && ui.scroll) { scroll += ui.scroll * (vgrid ? 1 : 3); layout(W, H); }
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
        if (ui.rpressed && !modal && ui_hover(&ui, SW, TB_H, L.w, H - TB_H - SB_H)) {
            int hit = my >= L.y ? item_at(mx, my, W) : -1;
            if (hit >= 0) { if (!ents[ord[hit]].sel) { sel_clear(); ents[ord[hit]].sel = true; } anchor = cur = hit; menu_open_at(mx, my, 1); }
            else { sel_clear(); menu_open_at(mx, my, 0); }
        }
        if (ui.rpressed && !modal && mx < SW && my > TB_H) {
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
                    int ix = vgrid ? c * CW + SP_3 : 0, iy = r * L.rh, iw = vgrid ? CW - 2 * SP_3 : L.w, ih = vgrid ? CW : L.rh;
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
        char tmp[PATH_MAX];

        /* toolbar */
        ui_toolbar(&ui, 0, 0, W);
        if (ui_icon_button(&ui, SP_3, ty, ICON_CHEVRON_LEFT, hpos > 0 ? 0 : UI_DISABLED, "Back · Alt+Left")) go_back();
        if (ui_icon_button(&ui, SP_3 + H_CTL + SP_2, ty, ICON_CHEVRON_RIGHT, hpos < nhist - 1 ? 0 : UI_DISABLED, "Forward · Alt+Right")) go_fwd();
        if (ui_icon_button(&ui, SP_3 + 2 * (H_CTL + SP_2), ty, ICON_ARROW_UP, strcmp(cwd, "/") ? 0 : UI_DISABLED, "Enclosing folder · Alt+Up")) go_up();
        if (ui.focus != path_id && cmaxw > 0) {
            int end = crumbs(&ui, cx0, ty, cmaxw);
            if (!modal && ui_clicked(&ui, end, ty, cx0 + cmaxw - end, H_CTL)) { ui.focus = path_id; path_fresh = true; snprintf(pathbox, sizeof pathbox, "%s", cwd); }
        }
        if (ui.focus == path_id) { if (ui_input(&ui, cx0, ty, cmaxw, H_CTL, pathbox, sizeof pathbox, "Type a path", UI_NOICON, 0)) { go(pathbox, true); ui.focus = 0; } }
        else snprintf(pathbox, sizeof pathbox, "%s", cwd);
        if (ui_search(&ui, srch_x, srch_y, srch_w, qbox, sizeof qbox, "Search")) ui.focus = 0;
        static const int view_icons[] = { ICON_GRID, ICON_LIST };
        int vsel = !vgrid;
        if (ui_segmented(&ui, seg_x, ty, view_icons, NULL, 2, &vsel)) { vgrid = !vsel; scroll = 0; relayout = true; }
        ui_tooltip(&ui, seg_x, ty, SP_1 + H_CTL, H_CTL, "Icons");
        ui_tooltip(&ui, seg_x + SP_1 + H_CTL, ty, SP_1 + H_CTL, H_CTL, "List");
        if (ui_icon_button(&ui, more_x, ty, ICON_MORE, 0, "More") && !modal) menu_open_at(more_x + H_CTL, ty + H_CTL + SP_2, 0);

        /* sidebar */
        if (SW) {
            ui_sidebar(&ui, 0, TB_H, SW, H - TB_H);
            int dpl = dragging ? place_at(mx, my) : -1;
            for (int i = 0; i < NPLACES; i++) {
                int y = place_y(i);
                if (!places[i].path) { ui_side_label(&ui, 0, y, places[i].label); continue; }
                char tr[12] = "";
                if (!strcmp(places[i].path, TRASH_FILES) && trash_n) snprintf(tr, sizeof tr, "%d", trash_n);
                if (ui_side_item(&ui, 0, y, SW, places[i].ico, places[i].label, tr[0] ? tr : NULL, !strcmp(cwd, places[i].path)) && !modal)
                    go(places[i].path, true);
                if (i == dpl) gfx_round_rect(c, SP_3, y, SW - 2 * SP_3, H_ROW, R_2, ui_th.accent_line);
            }
        }

        /* content */
        struct canvas cc = *c;
        canvas_clip(&cc, SW, L.y, L.w, L.h);
        if (relayout) { layout(W, H); relayout = false; }
        int date_w = gfx_text_width(T_BODY, "0000-00-00 00:00"), col_mod = W - SP_4 - date_w;
        int col_size = col_mod - SP_7, loc_w = L.w / 3 > 240 ? 240 : L.w / 3, col_loc = col_mod - SP_7 - loc_w;
        int name_x = SW + SP_4 + ICON_LG + SP_3, name_w = (intrash ? col_loc : col_size - 80) - SP_7 - name_x;
        if (!vgrid && nord) {
            for (int hi = 0; hi < 3; hi++) {
                const char *label = intrash ? (hi == 0 ? "Name" : hi == 1 ? "Original location" : "Deleted") : hi == 0 ? "Name" : hi == 1 ? "Size" : "Modified";
                int lw = gfx_text_width(T_CAPTION_B, label);
                int hx = hi == 0 ? name_x : hi == 2 ? col_mod : intrash ? col_loc : col_size - lw;
                bool hov = !(hi == 1 && intrash) && ui_hover(&ui, hx - SP_2, TB_H, lw + ICON_SM + SP_3, HDR_H);
                gfx_text(c, T_CAPTION_B, hx, TB_H + (HDR_H - T_CAPTION_B->height) / 2, label, hov ? UI_TEXT : UI_TEXT2);
                if (sort_key == hi && !(hi == 1 && intrash))
                    ui_icon(&ui, hx + lw + SP_1, TB_H + (HDR_H - ICON_SM) / 2, sort_desc ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP, ICON_SM, UI_TEXT2);
                if (ui.pressed && hov && !modal) do_action(A_SORTN + hi);
            }
            gfx_hline(c, SW, TB_H + HDR_H - 1, L.w, UI_LINE);
        }
        if (!nord) {                                                     /* empty states */
            int ey = TB_H, eh = H - SB_H - TB_H;
            if (nents) { if (ui_empty(&ui, SW, ey, L.w, eh, ICON_SEARCH, "No results", "Nothing in this folder matches your search.", "Clear search")) qbox[0] = 0; }
            else if (intrash) ui_empty(&ui, SW, ey, L.w, eh, ICON_TRASH, "Trash is empty", "Items you move to the Trash appear here.", NULL);
            else if (ui_empty(&ui, SW, ey, L.w, eh, ICON_FOLDER_OPEN, "This folder is empty", "Drop files here or create a new folder.", "New folder…")) do_action(A_NEWDIR);
        }
        struct canvas *oc = ui.c;
        ui.c = &cc;
        for (int j = scroll * L.cols; j < nord && j < (scroll + L.vis + 1) * L.cols; j++) {
            int x, y, iw, ih;
            item_rect(j, &x, &y, &iw, &ih);
            struct ent *e = &ents[ord[j]];
            bool hov = !modal && ui_hover(&ui, x, y, iw, ih) && mx < W - 12 && my >= L.y && my < L.y + L.h;
            bool dt = dragging && hov && e->dir && !e->sel;              /* a folder to drop onto */
            join(tmp, cwd, e->name);
            const struct aicon *ai = fileicon_get(ficon(tmp, e->dir, e->kind), vgrid ? ICON_APP : ICON_LG);
            const struct font *nf = e->sel ? T_BODY_B : T_BODY;
            if (vgrid) {
                if (e->sel) gfx_round_fill(&cc, x, y, iw, ih, R_2, UI_SELECTION);
                else if (hov) gfx_round_fill(&cc, x, y, iw, ih, R_2, ui_th.hover_ov);
                if (dt) gfx_round_rect(&cc, x, y, iw, ih, R_2, ui_th.accent_line);
                int ix = x + (CW - TH) / 2, iy = y + SP_3;
                struct thumb *t = NULL;
                if (e->kind == FK_IMAGE && !intrash) t = thumb_get(tmp, e->mtime, e->size);
                if (t) {
                    int tx = ix + (TH - t->w) / 2, tyy = iy + (TH - t->h) / 2;
                    gfx_blit(&cc, tx, tyy, t->px, t->w, t->h, t->w);
                    gfx_rect(&cc, tx - 1, tyy - 1, t->w + 2, t->h + 2, UI_LINE);
                } else ui_aicon(&ui, ix, iy, ai);
                centered_fit(&ui, nf, x + SP_2, iy + TH + 6, CW - 2 * SP_2, e->name, UI_TEXT);
            } else {
                if (e->sel) gfx_fill(&cc, x, y, iw, ih, UI_SELECTION);
                else if (hov) gfx_fill(&cc, x, y, iw, ih, ui_th.hover_ov);
                if (dt) gfx_rrect_ring(&cc, x, y, iw, ih, RADII(0, 0, 0, 0), 1, ui_th.accent_line);
                ui_aicon(&ui, x + SP_4, y + (ih - ICON_LG) / 2, ai);
                int tyy = y + (ih - T_BODY->height) / 2;
                ui_text_fit(&ui, nf, name_x, tyy, name_w, e->name, UI_TEXT);
                char b[PATH_MAX];
                if (intrash) ui_text_fit(&ui, T_BODY, col_loc, tyy, loc_w, e->orig ? e->orig : "Unknown", UI_TEXT2);
                else {
                    if (e->dir) snprintf(b, sizeof b, "—"); else fmt_size(b, sizeof b, e->size);
                    gfx_text(&cc, T_BODY, col_size - gfx_text_width(T_BODY, b), tyy, b, UI_TEXT2);
                }
                fmt_time(b, sizeof b, e->mtime);
                gfx_text(&cc, T_BODY, col_mod, tyy, b, UI_TEXT2);
            }
        }
        ui.c = oc;
        if (banding && (ui.buttons & 1)) {                               /* rubber band */
            int bx0 = band_x < mx - L.x ? band_x : mx - L.x, bx1 = band_x < mx - L.x ? mx - L.x : band_x;
            int cy = my - L.y + scroll * L.rh, by0 = band_y < cy ? band_y : cy, by1 = band_y < cy ? cy : band_y;
            int rx = L.x + bx0, ry = L.y + by0 - scroll * L.rh;
            gfx_fill(&cc, rx, ry, bx1 - bx0, by1 - by0, TH_A(ui_th.accent, 36));
            gfx_rect(&cc, rx, ry, bx1 - bx0, by1 - by0, ui_th.accent_line);
        }
        ui_vscroll(&ui, W - 12, L.y, L.h, L.nrows, L.vis, &scroll);

        /* status bar */
        char st[200], rt[64] = "";
        int ns = nsel();
        if (ns) {
            uint64_t tot = 0;
            for (int j = 0; j < nents; j++) if (ents[j].sel && !ents[j].dir) tot += ents[j].size;
            int i0 = first_sel();
            if (ns == 1) snprintf(st, sizeof st, "%d item%s, “%s” selected", nord, nord == 1 ? "" : "s", ents[i0].name);
            else snprintf(st, sizeof st, "%d items, %d selected", nord, ns);
            if (tot || !(ns == 1 && ents[i0].dir)) fmt_size(rt, sizeof rt, tot);
        } else snprintf(st, sizeof st, nord == nents ? (nord == 1 ? "%d item" : "%d items") : (nord == 1 ? "%d matching item" : "%d matching items"), nord);
        struct statfs sf;                                                /* free space of the folder's filesystem, right-aligned */
        if (statfs(cwd, &sf) == 0 && sf.f_blocks) {
            uint64_t fb = (uint64_t)sf.f_bavail * sf.f_bsize;
            char fr[24];
            if (fb >> 30) snprintf(fr, sizeof fr, "%u.%u GiB", (unsigned)(fb >> 30), (unsigned)((fb & 0x3FFFFFFF) * 10 >> 30));
            else fmt_size(fr, sizeof fr, fb);
            int l = strlen(rt);
            snprintf(rt + l, sizeof rt - l, "%s%s free", l ? "  ·  " : "", fr);
        }
        ui_status(&ui, SW, H - SB_H, W - SW, st, rt[0] ? rt : NULL);

        /* drag ghost */
        if (dragging) {
            int n = nsel(), f0 = first_sel();
            if (f0 >= 0) { join(tmp, cwd, ents[f0].name); ui_aicon(&ui, mx - 16, my - 16, fileicon_get(ficon(tmp, ents[f0].dir, ents[f0].kind), ICON_XL)); }
            if (n > 1) {
                char b[8];
                snprintf(b, sizeof b, "%d", n);
                int bw = gfx_text_width(T_CAPTION_B, b) + 2 * SP_2;
                if (bw < 18) bw = 18;
                gfx_round_fill(c, mx + 8, my - 22, bw, 18, 9, UI_ACCENT);
                gfx_text_center(c, T_CAPTION_B, mx + 8, my - 22 + (18 - T_CAPTION_B->height) / 2, bw, b, UI_ON_ACCENT);
            }
            if (ctrl()) {
                int pw = gfx_text_width(T_CAPTION_B, "Copy") + 2 * SP_3;
                gfx_round_fill(c, mx + 18, my + 6, pw, 20, R_2, ui_th.overlay);
                gfx_round_rect(c, mx + 18, my + 6, pw, 20, R_2, UI_LINE);
                gfx_text(c, T_CAPTION_B, mx + 18 + SP_3, my + 6 + (20 - T_CAPTION_B->height) / 2, "Copy", UI_TEXT);
            }
        }

        job_card(&ui, W, H);

        /* ---- dialogs and menu ---- */
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.dbl = sd; ui.rpressed = srp;
            ui.nkeys = nk; ui.scroll = ssc;
            memcpy(ui.keys, saved, sizeof saved);
        }
        if (mode != M_NONE) {
            bool fresh = !modal;                      /* opened this frame: the opening click must not close it */
            if (mode == M_MENU) {
                int chosen = menu_draw(&ui);
                if (chosen >= 0 || (!fresh && (ui.pressed || ui.rpressed))) mode = M_NONE;
                if (chosen >= 0) do_action(chosen);
                while (ui_key(&ui, &k)) if (k.key == KEY_ESC) mode = M_NONE;
            } else if (mode == M_NEWDIR || mode == M_NEWFILE || mode == M_RENAME) {
                int r = ui_prompt(&ui, mode == M_NEWDIR ? "New folder" : mode == M_NEWFILE ? "New text file" : "Rename", mode == M_RENAME ? "Rename" : "Create", namebuf, sizeof namebuf);
                int m0 = mode;
                if (r) mode = M_NONE;
                if (r == 1) { if (m0 == M_RENAME) do_rename(namebuf); else do_new(namebuf, m0 == M_NEWDIR); }
            } else if (mode == M_DELETE || mode == M_EMPTY) {
                char t[200];
                int n = nsel();
                if (mode == M_EMPTY) snprintf(t, sizeof t, "All %d item%s in the Trash will be deleted. This can't be undone.", trash_n, trash_n == 1 ? "" : "s");
                else if (n == 1) snprintf(t, sizeof t, "“%s”%s will be deleted. This can't be undone.", ents[first_sel()].name, ents[first_sel()].dir ? " and everything in it" : "");
                else snprintf(t, sizeof t, "%d items will be deleted. This can't be undone.", n);
                int r = ui_dialog(&ui, mode == M_EMPTY ? "Empty Trash?" : "Delete permanently?", t, "Cancel", mode == M_EMPTY ? "Empty Trash" : "Delete", UI_DANGER);
                int m0 = mode;
                if (r) mode = M_NONE;
                if (r == 1) { if (m0 == M_EMPTY) job_start(J_EMPTY, NULL, 0, NULL); else delete_sel(); }
            } else if (mode == M_PROPS) {
                if (props_draw(&ui)) mode = M_NONE;
            } else if (mode == M_CONFLICT) {
                int r = conflict_draw(&ui);
                if (r) { mode = M_NONE; J.all_ans = cf_all ? r : 0; J.answer = r; }
            }
        }
        if (msg_new) { ui_toast(&ui, msg, msg_undo && nundo ? "Undo" : NULL); msg_new = false; }
        const char *tn = fs_base(cwd)[0] ? fs_base(cwd) : "Root";
        for (int i = 0; i < NPLACES; i++) if (places[i].path && !strcmp(cwd, places[i].path)) tn = places[i].label;
        char nt[128];
        snprintf(nt, sizeof nt, "%s — Files", tn);
        if (strcmp(nt, title)) { gui_set_title(w, nt); snprintf(title, sizeof title, "%s", nt); }
        bool more = thumb_make();
        ui_end(&ui);
        thumb_busy = more;
    } while (ui_poll(&ui, thumb_busy ? 0 : J.active ? 50 : pend ? 60 : 1000));
    if (J.active) { J.cancel = 1; pthread_join(J.tid, NULL); }        /* closing the window cancels: the worker removes its partial copy */
    gui_window_destroy(w);
    return 0;
}
