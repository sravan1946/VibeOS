/* music [FILE...] - music player: the ~/Music library, a playlist, shuffle/repeat, seek bar, volume and album art
 * (embedded ID3 APIC). WAV, MP3 and Ogg Vorbis through user/music/adec.c.
 *
 * One /dev/dsp stream stays open for the session (an idle stream costs the kernel nothing). The UI loop keeps ~150 ms
 * queued, decoding a little at a time, so playback never needs a thread; the playing position is what was written
 * minus what is still queued (AUDIO_GETQ). Pause and seek drop the queue with AUDIO_FLUSH. The volume slider is this
 * stream's own volume (AUDIO_SETSVOL), so it doesn't touch Doom or the master volume.
 * ponytail: the library lists file names (tags are read only for the playing track); no playlist reordering or saving. */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <audio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <syscall.h>
#include "image.h"
#include "adec.h"

#define W 780
#define H 480
#define MAXT 512
#define ART 184
#define TARGET (AUDIO_RATE * 4 * 15 / 100)      /* bytes to keep queued: 150 ms */
#define CHUNK 1024                              /* frames per decode/write */

struct track { char *path; char label[96]; };
enum { REP_OFF, REP_ALL, REP_ONE };

static struct track lib[MAXT];
static int nlib;
static int pl[MAXT], npl;                       /* playlist: indices into lib */
static unsigned char played[MAXT];              /* shuffle: pl positions already played this round */
static int hist[64], nhist;                     /* shuffle: pl positions to go back through */
static int cur = -1;                            /* pl position of the loaded track */
static bool shuffle;
static int repeat = REP_OFF;

static int fd = -1;
static struct adec *dec;
static struct adec_tags tags;
static bool playing, eof;
static long written;                            /* frames written since base_ms */
static int base_ms, len_ms;
static int svol = 100;
static char status[128];
static uint32_t art[ART * ART];
static bool have_art;

static int cmp_track(const void *a, const void *b) { return strcasecmp(((const struct track *)a)->path, ((const struct track *)b)->path); }

static void lib_add(const char *path) {
    if (nlib == MAXT) return;
    for (int i = 0; i < nlib; i++) if (!strcmp(lib[i].path, path)) return;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    struct track *t = &lib[nlib++];
    t->path = strdup(path);
    snprintf(t->label, sizeof t->label, "%s", b);
    char *dot = strrchr(t->label, '.');
    if (dot) *dot = 0;
    for (char *p = t->label; *p; p++) if (*p == '_') *p = ' ';
}

static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 2) scan(p, depth + 1); }
        else if (adec_is_audio(de->d_name)) lib_add(p);
    }
    closedir(d);
}

static void rescan(void) {
    for (int i = 0; i < nlib; i++) free(lib[i].path);
    nlib = npl = nhist = 0;
    cur = -1;
    scan("/home/Music", 0);
    qsort(lib, nlib, sizeof *lib, cmp_track);
}

static void pl_add(int li) { if (npl < MAXT) { played[npl] = 0; pl[npl++] = li; } }

/* ---- playback ---- */
static void flush(void) { int z = 0; if (fd >= 0) ioctl(fd, AUDIO_FLUSH, &z); }

static void unload(void) {
    adec_close(dec);
    dec = NULL; playing = eof = have_art = false;
    tags = (struct adec_tags){ 0 };
    flush();
}

static int pos_ms(void) {
    if (!dec) return 0;
    long q = fd >= 0 ? audio_queued(fd) / 4 : 0;
    long p = base_ms + (written - q) * 1000 / AUDIO_RATE;
    return p < 0 ? 0 : p > len_ms ? len_ms : (int)p;
}

static void make_art(void) {
    struct image im;
    have_art = false;
    if (!tags.art || image_decode(tags.art, tags.art_len, &im) < 0) return;
    int m = im.w < im.h ? im.w : im.h;
    float sc = (float)ART / m;
    image_sample(&im, art, ART, ART, ART, (im.w - ART / sc) / 2, (im.h - ART / sc) / 2, sc, 1);   /* centre crop */
    image_free(&im);
    have_art = true;
}

static bool load(int pos) {
    unload();
    if (pos < 0 || pos >= npl) return false;
    const char *err;
    dec = adec_open(lib[pl[pos]].path, &tags, &err);
    if (!dec) { snprintf(status, sizeof status, "%s: %s", lib[pl[pos]].label, err); return false; }
    if (fd < 0 && (fd = audio_open("music")) < 0) { snprintf(status, sizeof status, "no audio device"); unload(); return false; }
    ioctl(fd, AUDIO_SETSVOL, &svol);
    cur = pos;
    played[pos] = 1;
    written = base_ms = 0;
    len_ms = adec_length_ms(dec);
    make_art();
    status[0] = 0;
    return true;
}

static void start(int pos) {
    if (load(pos)) playing = true;
}

static void seek(int ms) {
    if (!dec) return;
    if (ms < 0) ms = 0;
    if (ms >= len_ms) ms = len_ms > 100 ? len_ms - 100 : 0;
    flush();
    adec_seek(dec, ms);
    base_ms = ms; written = 0; eof = false;
}

static void pause_toggle(void) {
    if (!dec) { if (npl) start(cur >= 0 ? cur : 0); return; }
    if (playing) { int p = pos_ms(); playing = false; seek(p); }       /* drop the queue, remember where we were */
    else playing = true;
}

/* the pl position to go to, or -1 at the end of the list. auto_adv: the track ended by itself (repeat-one applies) */
static int next_pos(int dir, bool auto_adv) {
    if (!npl) return -1;
    if (auto_adv && repeat == REP_ONE && cur >= 0) return cur;
    if (shuffle) {
        if (dir < 0) { if (nhist) return hist[--nhist]; return cur; }
        int left = 0;
        for (int i = 0; i < npl; i++) left += !played[i];
        if (!left) {
            if (repeat != REP_ALL) return -1;
            memset(played, 0, npl);
            if (cur >= 0) played[cur] = 1;
            left = npl - (cur >= 0);
            if (!left) return cur;
        }
        int k = rand() % left;
        for (int i = 0; i < npl; i++) if (!played[i] && k-- == 0) return i;
    }
    int p = cur + dir;
    if (p < 0) return repeat == REP_ALL || !auto_adv ? npl - 1 : 0;
    if (p >= npl) return repeat == REP_ALL ? 0 : -1;
    return p;
}

static void go(int dir, bool auto_adv) {
    int was = cur;
    if (!auto_adv && dir < 0 && dec && pos_ms() > 3000) { seek(0); return; }     /* "previous" restarts a track that's under way */
    int p = next_pos(dir, auto_adv);
    if (p < 0) { unload(); base_ms = 0; return; }
    if (shuffle && dir > 0 && was >= 0 && nhist < 64) hist[nhist++] = was;
    start(p);
}

static void play_lib(int li) {
    int p = -1;
    for (int i = 0; i < npl; i++) if (pl[i] == li) p = i;
    if (p < 0) { pl_add(li); p = npl - 1; }
    nhist = 0;
    start(p);
}

static void pl_remove(int p) {
    if (p < 0 || p >= npl) return;
    for (int i = p; i < npl - 1; i++) { pl[i] = pl[i + 1]; played[i] = played[i + 1]; }
    npl--; nhist = 0;
    if (p == cur) { unload(); cur = -1; }
    else if (p < cur) cur--;
}

/* keep the stream topped up; returns once ~150 ms are queued */
static void fill(void) {
    static int16_t buf[CHUNK * 2];
    if (!playing || eof || !dec) return;
    for (int n = 0; n < 8 && audio_queued(fd) < TARGET; n++) {
        int got = adec_read(dec, buf, CHUNK);
        if (got <= 0) { eof = true; break; }
        write(fd, buf, got * 4);
        written += got;
    }
}

/* ---- drawing (docs/DESIGN.md §9): tabs + list rows on the left, now playing on the right, status bar ---- */
#define LW 300                                  /* list pane width */
#define RX (LW + 1 + SP_7)                      /* now-playing column */
#define RW (W - RX - SP_7)
#define LIST_Y (H_TAB + SP_2)
#define FOOT_Y (H - H_STATUS - SP_3 - H_CTL)
#define LIST_H (FOOT_Y - SP_3 - LIST_Y)

static void fmt_time(char *b, int n, int ms) { snprintf(b, n, "%d:%02d", ms / 60000, ms / 1000 % 60); }

/* a list of 28px rows; keys come from the app (Up/Down/Home/End/PgUp/PgDn move *sel). Returns the row double-clicked or -1 */
static int track_list(struct ui *ui, const int *idx, int n, int playing_pos, int *sel, int *scroll) {
    int vis = LIST_H / H_ROW, act = -1;
    if (ui_hover(ui, 0, LIST_Y, LW, LIST_H) && ui->scroll) *scroll += ui->scroll * 3;
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + vis) *scroll = *sel - vis + 1;
    if (*scroll > n - vis) *scroll = n - vis;
    if (*scroll < 0) *scroll = 0;
    struct canvas cc = *ui->c, *o = ui->c;
    canvas_clip(&cc, 0, LIST_Y, LW, LIST_H);
    ui->c = &cc;
    for (int i = *scroll; i < n && i < *scroll + vis + 1; i++) {
        int y = LIST_Y + (i - *scroll) * H_ROW;
        bool cur_row = i == playing_pos;
        if (ui_row(ui, SP_3, y, LW - 2 * SP_3, cur_row ? (playing ? ICON_VOLUME_HIGH : ICON_PAUSE) : ICON_MUSIC, NULL, lib[idx[i]].label, NULL, NULL,
                   i == *sel ? UI_SELECTED : 0) && y + H_ROW <= LIST_Y + LIST_H) { *sel = i; if (ui->dbl) act = i; }
    }
    ui->c = o;
    ui_vscroll(ui, LW - 12, LIST_Y, LIST_H, n, vis, scroll);
    return act;
}

int main(int argc, char **argv) {
    srand(uptime_ms());
    rescan();
    for (int i = 1; i < argc; i++) lib_add(argv[i]);
    if (argc > 1) {                             /* explicit files: the playlist is exactly those, and they start playing */
        for (int i = 1; i < argc; i++) for (int j = 0; j < nlib; j++) if (!strcmp(lib[j].path, argv[i])) pl_add(j);
    } else for (int i = 0; i < nlib; i++) pl_add(i);
    struct gui_window *w = gui_window_create("Music", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("music"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    if (argc > 1 && npl) start(0);

    static const char *const tabs[] = { "Library", "Playlist" };
    static int libi[MAXT];
    for (int i = 0; i < MAXT; i++) libi[i] = i;
    int tab = 0, sel[2] = { 0, 0 }, scroll[2] = { 0, 0 };
    bool seeking = false;
    int seekv = 0, last_pos = -1;
    uint32_t last_draw = 0;
    char title[128] = "", shown[128] = "", shown_status[128] = "";
    do {
        /* ---- keys (Space/Enter/arrows go to a widget that has keyboard focus instead) ---- */
        int nrows = tab == 0 ? nlib : npl, act = -1;
        for (int i = 0; i < ui.nkeys; i++) {
            struct gui_event *e = &ui.keys[i];
            if (e->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) continue;
            int k = e->key | 0x20, *s = &sel[tab];
            bool nav = e->key == ' ' || e->key == KEY_ENTER || (e->key >= KEY_UP && e->key <= KEY_PGDN);
            if (nav && ui.focus) continue;
            if (e->key == ' ') pause_toggle();
            else if (e->key == KEY_RIGHT) seek(pos_ms() + 5000);
            else if (e->key == KEY_LEFT) seek(pos_ms() - 5000);
            else if (e->key == KEY_DOWN) (*s)++;
            else if (e->key == KEY_UP) (*s)--;
            else if (e->key == KEY_PGDN) *s += LIST_H / H_ROW;
            else if (e->key == KEY_PGUP) *s -= LIST_H / H_ROW;
            else if (e->key == KEY_HOME) *s = 0;
            else if (e->key == KEY_END) *s = nrows - 1;
            else if (e->key == KEY_ENTER && nrows) act = *s;
            else if (k == 'n') go(1, false);
            else if (k == 'p') go(-1, false);
            else if (k == 's') shuffle = !shuffle;
            else if (k == 'r') repeat = (repeat + 1) % 3;
            else if (e->key == '+' || e->key == '=') { svol = svol > 95 ? 100 : svol + 5; if (fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol); }
            else if (e->key == '-') { svol = svol < 5 ? 0 : svol - 5; if (fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol); }
            else continue;
            memmove(ui.keys + i, ui.keys + i + 1, (--ui.nkeys - i) * sizeof ui.keys[0]);
            i--;
        }
        fill();
        if (playing && eof && audio_queued(fd) == 0) go(1, true);          /* the track played out */

        uint32_t now = uptime_ms();
        int pos = pos_ms();
        if (!ui.timeout || now - last_draw >= 100 || pos / 1000 != last_pos / 1000) {
            last_draw = now; last_pos = pos;
            ui_clear(&ui);
            struct canvas *c = ui.c;
            /* ---- left: library / playlist ---- */
            ui_tabbar(&ui, 0, 0, LW, tabs, 2, &tab, 0);
            ui_vsep(&ui, LW, 0, H - H_STATUS);
            int fb = 0;                                                    /* footer buttons, 8 apart */
            const char *b0 = tab ? "Remove" : "Add", *b1 = tab ? "Clear" : "Add all", *b2 = tab ? NULL : "Refresh";
            int w0 = ui_button_w(UI_NOICON, b0), w1 = ui_button_w(UI_NOICON, b1);
            if (tab == 0) {
                int a = track_list(&ui, libi, nlib, cur >= 0 && dec ? pl[cur] : -1, &sel[0], &scroll[0]);
                if (act < 0) act = a;
                if (act >= 0) play_lib(act);
                if (ui_button(&ui, SP_5, FOOT_Y, w0, H_CTL, b0, UI_SECONDARY | (nlib ? 0 : UI_DISABLED))) fb = 1;
                if (ui_button(&ui, SP_5 + w0 + SP_3, FOOT_Y, w1, H_CTL, b1, UI_SECONDARY | (nlib ? 0 : UI_DISABLED))) fb = 2;
                if (ui_button(&ui, SP_5 + w0 + w1 + 2 * SP_3, FOOT_Y, ui_button_w(UI_NOICON, b2), H_CTL, b2, UI_SECONDARY)) fb = 3;
                if (fb == 1) pl_add(sel[0]);
                if (fb == 2) for (int i = 0; i < nlib; i++) pl_add(i);
                if (fb == 3) { unload(); rescan(); for (int i = 0; i < nlib; i++) pl_add(i); }
                if (!nlib) ui_empty(&ui, 0, LIST_Y, LW, LIST_H, ICON_MUSIC, "No music yet", "Put WAV, MP3 or Ogg files in ~/Music, then refresh.", NULL);
            } else {
                int a = track_list(&ui, pl, npl, cur, &sel[1], &scroll[1]);
                if (act < 0) act = a;
                if (act >= 0) { nhist = 0; start(act); }
                if (ui_button(&ui, SP_5, FOOT_Y, w0, H_CTL, b0, UI_SECONDARY | (npl ? 0 : UI_DISABLED))) pl_remove(sel[1]);
                if (ui_button(&ui, SP_5 + w0 + SP_3, FOOT_Y, w1, H_CTL, b1, UI_SECONDARY | (npl ? 0 : UI_DISABLED))) { while (npl) pl_remove(npl - 1); }
                if (!npl) ui_empty(&ui, 0, LIST_Y, LW, LIST_H, ICON_PLAYLIST, "The playlist is empty", "Add tracks from the library.", NULL);
            }
            /* ---- right: now playing ---- */
            int ay = (H - H_STATUS - (ART + SP_6 + HIT_MIN + SP_5 + H_CTL + SP_7 + H_CTL)) / 2;   /* the column, centred */
            if (have_art) gfx_blit(c, RX, ay, art, ART, ART, ART);
            else {                                                         /* no cover: a quiet placeholder */
                gfx_round_fill(c, RX, ay, ART, ART, R_3, UI_SURFACE);
                ui_icon(&ui, RX + (ART - ICON_APP) / 2, ay + (ART - ICON_APP) / 2, ICON_MUSIC, ICON_APP, UI_TEXT3);
            }
            gfx_ring_out(c, RX, ay, ART, ART, have_art ? 0 : R_3, UI_LINE);
            int tx = RX + ART + SP_6, tw = W - SP_7 - tx, ty = ay + SP_3;
            const char *name = cur >= 0 && dec ? (tags.title[0] ? tags.title : lib[pl[cur]].label) : "Nothing playing";
            ui_text_fit(&ui, T_TITLE, tx, ty, tw, name, dec ? UI_TEXT : UI_TEXT2);
            ty += T_TITLE->height + SP_2;
            if (dec) {
                if (tags.artist[0]) { ui_text_fit(&ui, T_BODY, tx, ty, tw, tags.artist, UI_TEXT); ty += T_BODY->height; }
                if (tags.album[0]) { ui_text_fit(&ui, T_BODY, tx, ty, tw, tags.album, UI_TEXT2); ty += T_BODY->height; }
                ty += SP_4;
                const char *inf = adec_info(dec), *comma = strchr(inf, ',');      /* "MP3" / "44100 Hz, stereo, 64 kbps" */
                char b[96];
                snprintf(b, sizeof b, "%.*s", comma ? (int)(comma - inf) : (int)strlen(inf), inf);
                ui_text_fit(&ui, T_CAPTION_B, tx, ty, tw, b, UI_TEXT2);
                if (comma) ui_text_fit(&ui, T_CAPTION, tx, ty + T_CAPTION->height, tw, comma + 2, UI_TEXT2);
            } else ui_text_fit(&ui, T_BODY, tx, ty, tw, npl ? "Double-click a track or press Space" : "Add tracks to the playlist", UI_TEXT2);
            if (dec) snprintf(title, sizeof title, "%s — Music", name); else strcpy(title, "Music");
            /* seek bar */
            char tb[16];
            int sy = ay + ART + SP_6, tl = gfx_text_width(T_CAPTION, "00:00");
            if (!seeking) seekv = len_ms ? (int)((int64_t)pos * 1000 / len_ms) : 0;
            fmt_time(tb, sizeof tb, seeking ? (int)((int64_t)seekv * len_ms / 1000) : pos);
            gfx_text(c, T_CAPTION, RX, sy + (HIT_MIN - T_CAPTION->height) / 2, tb, UI_TEXT2);
            if (ui_slider_ex(&ui, RX + tl + SP_4, sy, RW - 2 * (tl + SP_4), &seekv, 0, 1000, dec ? 0 : UI_DISABLED) && dec) seeking = true;
            if (seeking && ui.released) { seek((int)((int64_t)seekv * len_ms / 1000)); seeking = false; }
            fmt_time(tb, sizeof tb, len_ms);
            gfx_text(c, T_CAPTION, RX + RW - gfx_text_width(T_CAPTION, tb), sy + (HIT_MIN - T_CAPTION->height) / 2, tb, UI_TEXT2);
            /* transport: shuffle, previous, play/pause (the primary action), next, repeat */
            static const char *const rep[] = { "Repeat: off · R", "Repeat all · R", "Repeat one · R" };
            int pw = H_CTL + 2 * SP_4, bx = RX + (RW - (4 * H_CTL + pw + 4 * SP_3)) / 2, by = sy + HIT_MIN + SP_5;
            if (ui_icon_button(&ui, bx, by, ICON_SHUFFLE, shuffle ? UI_ON : 0, shuffle ? "Shuffle: on · S" : "Shuffle: off · S")) shuffle = !shuffle;
            bx += H_CTL + SP_3;
            if (ui_icon_button(&ui, bx, by, ICON_PREV, npl ? 0 : UI_DISABLED, "Previous · P")) go(-1, false);
            bx += H_CTL + SP_3;
            if (ui_button_ex(&ui, bx, by, pw, H_CTL, playing ? ICON_PAUSE : ICON_PLAY, NULL, UI_PRIMARY | (npl ? 0 : UI_DISABLED))) pause_toggle();
            ui_tooltip(&ui, bx, by, pw, H_CTL, playing ? "Pause · Space" : "Play · Space");
            bx += pw + SP_3;
            if (ui_icon_button(&ui, bx, by, ICON_NEXT, npl ? 0 : UI_DISABLED, "Next · N")) go(1, false);
            bx += H_CTL + SP_3;
            if (ui_icon_button(&ui, bx, by, repeat == REP_ONE ? ICON_REPEAT_ONE : ICON_REPEAT, repeat != REP_OFF ? UI_ON : 0, rep[repeat])) repeat = (repeat + 1) % 3;
            /* volume */
            char vb[8];
            snprintf(vb, sizeof vb, "%d%%", svol);
            int vw = gfx_text_width(T_BODY, "100%");
            int vy = by + H_CTL + SP_7;
            ui_icon(&ui, RX, vy + (H_CTL - ICON_SM) / 2, svol == 0 ? ICON_VOLUME_MUTE : svol < 50 ? ICON_VOLUME_LOW : ICON_VOLUME_HIGH, ICON_SM, UI_TEXT2);
            if (ui_slider(&ui, RX + ICON_SM + SP_4, vy + (H_CTL - HIT_MIN) / 2, RW - ICON_SM - 2 * SP_4 - vw, &svol, 0, 100) && fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol);
            gfx_text(c, T_BODY, RX + RW - gfx_text_width(T_BODY, vb), vy + (H_CTL - T_BODY->height) / 2, vb, UI_TEXT);
            /* status bar; errors also as a toast */
            char sl[48] = "", sr[64] = "";
            if (dec) snprintf(sl, sizeof sl, "Track %d of %d", cur + 1, npl);
            else snprintf(sl, sizeof sl, "%d track%s in the playlist", npl, npl == 1 ? "" : "s");
            snprintf(sr, sizeof sr, "%d in the library", nlib);
            ui_status(&ui, 0, H - H_STATUS, W, status[0] ? status : sl, sr);
            if (strcmp(status, shown_status)) { if (status[0]) ui_toast(&ui, status, NULL); strcpy(shown_status, status); }
            ui_end(&ui);
            if (strcmp(title, shown)) { gui_set_title(w, title); strcpy(shown, title); }
        }
    } while (ui_poll(&ui, playing ? 25 : -1));
    unload();
    if (fd >= 0) close(fd);
    gui_window_destroy(w);
    return 0;
}
