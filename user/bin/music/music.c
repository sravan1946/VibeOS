/* music [FILE...] - music player: the ~/Music library, a playlist, shuffle/repeat, seek bar, volume and album art
 * (embedded ID3 APIC). WAV, MP3 and Ogg Vorbis through user/music/adec.c.
 *
 * One /dev/dsp stream stays open for the session (an idle stream costs the kernel nothing). The UI loop keeps ~150 ms
 * queued, decoding a little at a time, so playback never needs a thread; the playing position is what was written
 * minus what is still queued (AUDIO_GETQ). Pause and seek drop the queue with AUDIO_FLUSH. The volume slider is this
 * stream's own volume (AUDIO_SETSVOL), so it doesn't touch Doom or the master volume.
 * ponytail: the library lists file names (tags are read only for the playing track); no playlist reordering or saving. */
#include <ui.h>
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

/* ---- drawing ---- */
static void tri(struct canvas *c, int x0, int y0, int x1, int y1, int x2, int y2, color_t col) {
    int lx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), hx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int ly = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), hy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = ly; y <= hy; y++)
        for (int x = lx; x <= hx; x++) {
            int d0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0), d1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1), d2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) gfx_pixel(c, x, y, col);
        }
    gfx_line_aa(c, x0, y0, x1, y1, 1.2f, col); gfx_line_aa(c, x1, y1, x2, y2, 1.2f, col); gfx_line_aa(c, x2, y2, x0, y0, 1.2f, col);
}

enum { I_PLAY, I_PAUSE, I_NEXT, I_PREV };
static void icon(struct canvas *c, int kind, int cx, int cy, int s, color_t col) {
    switch (kind) {
    case I_PLAY: tri(c, cx - s * 2 / 3, cy - s, cx - s * 2 / 3, cy + s, cx + s, cy, col); break;
    case I_PAUSE: gfx_fill(c, cx - s * 2 / 3, cy - s, s / 2 + 1, 2 * s + 1, col); gfx_fill(c, cx + s / 6, cy - s, s / 2 + 1, 2 * s + 1, col); break;
    case I_NEXT: tri(c, cx - s, cy - s, cx - s, cy + s, cx + s / 2, cy, col); gfx_fill(c, cx + s / 2 + 1, cy - s, 3, 2 * s + 1, col); break;
    case I_PREV: tri(c, cx + s, cy - s, cx + s, cy + s, cx - s / 2, cy, col); gfx_fill(c, cx - s / 2 - 3, cy - s, 3, 2 * s + 1, col); break;
    }
}

static void note_art(struct canvas *c, int x, int y) {      /* the cover when a track has none */
    gfx_vgradient(c, x, y, ART, ART, RGB(0x3A, 0x2A, 0x6E), RGB(0x1E, 0x1C, 0x3D));
    color_t k = RGB(0xB3, 0x88, 0xFF);
    gfx_circle_fill(c, x + 70, y + 126, 20, k);
    gfx_circle_fill(c, x + 118, y + 114, 20, k);
    gfx_fill(c, x + 86, y + 52, 6, 76, k);
    gfx_fill(c, x + 134, y + 40, 6, 76, k);
    gfx_fill(c, x + 86, y + 44, 54, 16, k);
}

static void fmt_time(char *b, int n, int ms) { snprintf(b, n, "%d:%02d", ms / 60000, ms / 1000 % 60); }

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
    static char plbuf[MAXT][112];
    static const char *plp[MAXT], *libp[MAXT];
    int tab = 0, sel[2] = { 0, 0 }, scroll[2] = { 0, 0 };
    bool seeking = false;
    int seekv = 0, last_pos = -1;
    uint32_t last_draw = 0;
    char title[128] = "", shown[128] = "";
    do {
        /* ---- keys (the lists keep up/down/enter for themselves) ---- */
        for (int i = 0; i < ui.nkeys; i++) {
            struct gui_event *e = &ui.keys[i];
            if (e->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) continue;
            int k = e->key | 0x20;
            if (e->key == ' ') pause_toggle();
            else if (e->key == KEY_RIGHT) seek(pos_ms() + 5000);
            else if (e->key == KEY_LEFT) seek(pos_ms() - 5000);
            else if (k == 'n') go(1, false);
            else if (k == 'p') go(-1, false);
            else if (k == 's') shuffle = !shuffle;
            else if (k == 'r') repeat = (repeat + 1) % 3;
            else if (e->key == '+' || e->key == '=') { svol = svol > 95 ? 100 : svol + 5; if (fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol); }
            else if (e->key == '-') { svol = svol < 5 ? 0 : svol - 5; if (fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol); }
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
            ui_tabs(&ui, 16, 16, 290, 32, tabs, 2, &tab);
            for (int i = 0; i < nlib; i++) libp[i] = lib[i].label;
            for (int i = 0; i < npl; i++) {
                snprintf(plbuf[i], sizeof plbuf[i], "%s%s", i == cur ? "> " : "", lib[pl[i]].label);
                plp[i] = plbuf[i];
            }
            int act = tab == 0 ? ui_list(&ui, 16, 58, 290, 330, libp, nlib, &sel[0], &scroll[0])
                               : ui_list(&ui, 16, 58, 290, 330, plp, npl, &sel[1], &scroll[1]);
            if (tab == 0) {
                if (act >= 0) play_lib(act);
                if (ui_button(&ui, 16, 400, 84, 34, "Add", UI_NORMAL_BTN) && nlib) pl_add(sel[0]);
                if (ui_button(&ui, 108, 400, 96, 34, "Add all", UI_NORMAL_BTN)) for (int i = 0; i < nlib; i++) pl_add(i);
                if (ui_button(&ui, 212, 400, 94, 34, "Refresh", UI_NORMAL_BTN)) { unload(); rescan(); for (int i = 0; i < nlib; i++) pl_add(i); }
                if (!nlib) gfx_text_center(c, &font_ui, 16, 200, 290, "No music in ~/Music", UI_TEXT_DIM);
            } else {
                if (act >= 0) { nhist = 0; start(act); }
                if (ui_button(&ui, 16, 400, 120, 34, "Remove", UI_NORMAL_BTN)) pl_remove(sel[1]);
                if (ui_button(&ui, 144, 400, 120, 34, "Clear", UI_NORMAL_BTN)) { while (npl) pl_remove(npl - 1); }
                if (!npl) gfx_text_center(c, &font_ui, 16, 200, 290, "The playlist is empty", UI_TEXT_DIM);
            }
            /* ---- right: now playing ---- */
            ui_panel(&ui, 324, 16, 440, 418);
            if (have_art) gfx_blit(c, 340, 32, art, ART, ART, ART); else note_art(c, 340, 32);
            gfx_rect(c, 340, 32, ART, ART, UI_BORDER);
            const char *name = cur >= 0 && dec ? (tags.title[0] ? tags.title : lib[pl[cur]].label) : "Nothing playing";
            ui_text_fit(&ui, &font_title, 540, 44, 210, name, UI_TEXT);
            if (dec) {
                if (tags.artist[0]) ui_text_fit(&ui, &font_ui, 540, 82, 210, tags.artist, UI_ACCENT);
                if (tags.album[0]) ui_text_fit(&ui, &font_ui, 540, 106, 210, tags.album, UI_TEXT_DIM);
                const char *inf = adec_info(dec), *comma = strchr(inf, ',');      /* "MP3" / "44100 Hz, stereo, 64 kbps" */
                gfx_text_n(c, &font_ui_bold, 540, 140, inf, comma ? (int)(comma - inf) : (int)strlen(inf), UI_TEXT);
                if (comma) ui_text_fit(&ui, &font_ui, 540, 164, 210, comma + 2, UI_TEXT_DIM);
                char b[48];
                snprintf(b, sizeof b, "Track %d of %d", cur + 1, npl);
                ui_text_fit(&ui, &font_ui, 540, 188, 210, b, UI_TEXT_DIM);
            }
            if (dec) snprintf(title, sizeof title, "%s - Music", name); else strcpy(title, "Music");
            /* seek bar */
            char tb[16];
            if (!seeking) seekv = len_ms ? (int)((int64_t)pos * 1000 / len_ms) : 0;
            fmt_time(tb, sizeof tb, seeking ? (int)((int64_t)seekv * len_ms / 1000) : pos);
            gfx_text(c, &font_ui, 340, 240, tb, UI_TEXT_DIM);
            if (ui_slider(&ui, 392, 236, 312, &seekv, 0, 1000) && dec) seeking = true;
            if (seeking && ui.released) { seek((int)((int64_t)seekv * len_ms / 1000)); seeking = false; }
            fmt_time(tb, sizeof tb, len_ms);
            gfx_text(c, &font_ui, 724, 240, tb, UI_TEXT_DIM);
            /* transport */
            if (ui_tool(&ui, 436, 276, 56, 56, "", false)) go(-1, false);
            icon(c, I_PREV, 464, 304, 10, UI_TEXT);
            if (ui_clicked(&ui, 508, 270, 68, 68)) pause_toggle();
            gfx_circle_fill(c, 542, 304, 30, ui_hover(&ui, 508, 270, 68, 68) ? RGB(0xC4, 0xA0, 0xFF) : UI_ACCENT);
            icon(c, playing ? I_PAUSE : I_PLAY, 542 + (playing ? 0 : 2), 304, 12, RGB(0x1A, 0x10, 0x2E));
            if (ui_tool(&ui, 592, 276, 56, 56, "", false)) go(1, false);
            icon(c, I_NEXT, 620, 304, 10, UI_TEXT);
            /* shuffle, repeat, volume */
            static const char *const rep[] = { "Repeat: off", "Repeat: all", "Repeat: one" };
            gfx_round_rect(c, 340, 356, 100, 32, 6, UI_BORDER); gfx_round_rect(c, 448, 356, 130, 32, 6, UI_BORDER);
            if (ui_tool(&ui, 340, 356, 100, 32, "Shuffle", shuffle)) shuffle = !shuffle;
            if (ui_tool(&ui, 448, 356, 130, 32, rep[repeat], repeat != REP_OFF)) repeat = (repeat + 1) % 3;
            gfx_text(c, &font_ui, 340, 400, "Volume", UI_TEXT_DIM);
            if (ui_slider(&ui, 404, 396, 250, &svol, 0, 100) && fd >= 0) ioctl(fd, AUDIO_SETSVOL, &svol);
            char vb[8];
            snprintf(vb, sizeof vb, "%d%%", svol);
            gfx_text(c, &font_ui, 676, 400, vb, UI_TEXT);
            /* footer */
            if (status[0]) ui_text_fit(&ui, &font_ui, 16, 448, W - 32, status, UI_BAD);
            else gfx_text(c, &font_ui, 16, 448, "Space: play/pause   Left/Right: seek 5 s   N / P: next / previous   S: shuffle   R: repeat   + / -: volume", UI_TEXT_DIM);
            ui_end(&ui);
            if (strcmp(title, shown)) { gui_set_title(w, title); strcpy(shown, title); }
        }
    } while (ui_poll(&ui, playing ? 25 : -1));
    unload();
    if (fd >= 0) close(fd);
    gui_window_destroy(w);
    return 0;
}
