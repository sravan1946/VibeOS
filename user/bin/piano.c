/* piano - two-octave on-screen keyboard (mouse or A W S E D F T G Y H U J K O L P ;) with a polyphonic
 * synth: sine/square/saw/triangle, ADSR envelope, scope. Audio is generated in 10 ms chunks and written to
 * /dev/dsp only while fewer than ~30 ms are queued, so the UI never blocks on the sound card and latency stays low.
 * ponytail: naive (aliasing) square/saw, no oscillator interpolation; fine below ~2 kHz. */
#include <ui.h>
#include <audio.h>
#include <math.h>
#include <stdlib.h>
#include <syscall.h>

#define W 760
#define H 440
#define CH 480                       /* frames per chunk = 10 ms */
#define QTARGET (3 * CH * 4)         /* bytes to keep queued */
#define NV 16
#define NKEYS 24
#define KX 30
#define KY 196
#define KW 50
#define KH 200
#define BW 32
#define BH 124

enum { OFF, ATK, DEC, SUS, REL };
struct voice { int midi, src, stage; float lev, rel, ph, inc; };
static struct voice vo[NV];
static int wave_sel, oct = 4, vol = 80, A = 10, D = 200, S = 60, R = 250;
static float sinetab[1024], scope[2048];
static unsigned scope_pos;

static const char *KEYS = "awsedftgyhujkolp;";              /* semitone offsets 0..16 from the left C */
static const int BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
static const int WIDX[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };   /* white key a semitone sits on / after */
static const int WSEMI[7] = { 0, 2, 4, 5, 7, 9, 11 };

static int base(void) { return 12 * (oct + 1); }            /* MIDI number of the left C */

static void note_on(int midi, int src) {
    struct voice *v = 0;
    for (int i = 0; i < NV; i++) {
        if (vo[i].stage && vo[i].src == src && src) return;   /* key auto-repeat */
        if (!v && !vo[i].stage) v = &vo[i];
    }
    if (!v) {                                                 /* steal the quietest voice */
        v = &vo[0];
        for (int i = 1; i < NV; i++) if (vo[i].lev < v->lev) v = &vo[i];
    }
    *v = (struct voice){ .midi = midi, .src = src, .stage = ATK, .inc = 440.0 * pow(2, (midi - 69) / 12.0) / AUDIO_RATE };
}

static void note_off(int src) {
    for (int i = 0; i < NV; i++) {
        struct voice *v = &vo[i];
        if (!v->stage || v->stage == REL || v->src != src) continue;
        if (v->lev < 0.2f) v->lev = 0.2f;                     /* a very short tap is still audible */
        v->rel = v->lev / (R * 48.f);
        v->stage = REL; v->src = 0;
    }
}

static bool any_voice(void) { for (int i = 0; i < NV; i++) if (vo[i].stage) return true; return false; }
static bool held(int midi) { for (int i = 0; i < NV; i++) if (vo[i].stage && vo[i].stage != REL && vo[i].midi == midi) return true; return false; }

/* one chunk of stereo s16 */
static void gen(short *out) {
    float ga = 1.f / (A * 48), sus = S / 100.f, gd = (1 - sus) / (D * 48);
    for (int i = 0; i < CH; i++) {
        float mix = 0;
        for (int k = 0; k < NV; k++) {
            struct voice *v = &vo[k];
            if (!v->stage) continue;
            if (v->stage == ATK) { v->lev += ga; if (v->lev >= 1) { v->lev = 1; v->stage = DEC; } }
            else if (v->stage == DEC) { v->lev -= gd; if (v->lev <= sus) { v->lev = sus; v->stage = SUS; } }
            else if (v->stage == REL) { v->lev -= v->rel; if (v->lev <= 0) { v->stage = OFF; continue; } }
            float p = v->ph, o;
            switch (wave_sel) {
            case 0: o = sinetab[(int)(p * 1024) & 1023]; break;
            case 1: o = p < 0.5f ? 1 : -1; break;
            case 2: o = 2 * p - 1; break;
            default: o = p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
            }
            mix += o * v->lev;
            v->ph += v->inc;
            if (v->ph >= 1) v->ph -= 1;
        }
        mix *= 0.25f;
        if (mix > 1) mix = 1; else if (mix < -1) mix = -1;
        out[2 * i] = out[2 * i + 1] = (short)(mix * 32767);
        scope[scope_pos++ & 2047] = mix;
    }
}

static int keyx(int s) {                                      /* left edge of key `s` (0..23) on screen */
    int w = s / 12 * 7 + WIDX[s % 12];
    return BLACK[s % 12] ? KX + (w + 1) * KW - BW / 2 : KX + w * KW;
}

static int key_at(int mx, int my) {                           /* MIDI note under the pointer or -1 */
    if (mx < KX || my < KY || mx >= KX + 14 * KW || my >= KY + KH) return -1;
    if (my < KY + BH)
        for (int s = 0; s < NKEYS; s++)
            if (BLACK[s % 12] && mx >= keyx(s) && mx < keyx(s) + BW) return base() + s;
    int w = (mx - KX) / KW;
    return base() + WSEMI[w % 7] + 12 * (w / 7);
}

static void draw_keys(struct ui *ui) {
    struct canvas *c = ui->c;
    for (int pass = 0; pass < 2; pass++)                      /* whites, then blacks on top */
        for (int s = 0; s < NKEYS; s++) {
            int blk = BLACK[s % 12];
            if (blk != pass) continue;
            int x = keyx(s), w = blk ? BW : KW - 2, h = blk ? BH : KH;
            bool on = held(base() + s);
            color_t col = blk ? (on ? UI_ACCENT : RGB(0x26, 0x22, 0x3A)) : (on ? mix(RGB(0xEE, 0xEC, 0xF8), UI_ACCENT, 150) : RGB(0xEE, 0xEC, 0xF8));
            gfx_round_fill(c, x, KY, w, h, 6, col);
            if (blk) gfx_round_rect(c, x, KY, w, h, 6, UI_BORDER);
            if (s < 17) {
                char l[2] = { KEYS[s] - 32, 0 };
                if (KEYS[s] == ';') l[0] = ';';
                gfx_text_center(c, &font_ui_bold, x, KY + h - 26, w, l, blk ? UI_TEXT_DIM : RGB(0x6A, 0x66, 0x88));
            }
            if (!blk && s % 12 == 0) {
                char n[8];
                snprintf(n, sizeof n, "C%d", oct + s / 12);
                gfx_text_center(c, &font_ui, x, KY + h - 52, w, n, RGB(0x9A, 0x96, 0xB8));
            }
        }
}

static void draw_scope(struct ui *ui, int x, int y, int w, int h) {
    struct canvas *c = ui->c;
    ui_panel(ui, x, y, w, h);
    int cy = y + h / 2;
    gfx_fill(c, x + 8, cy, w - 16, 1, RGBA(255, 255, 255, 30));
    float s[1600];
    for (int i = 0; i < 1600; i++) s[i] = scope[(scope_pos - 1600 + i) & 2047];
    int t = 1;
    while (t < 600 && !(s[t - 1] < 0 && s[t] >= 0)) t++;      /* trigger on a rising zero crossing */
    if (t >= 600) t = 0;
    int px = x + 8, py = cy;
    for (int j = 0; j < w - 16 && t + 2 * j < 1600; j++) {
        int yy = cy - (int)(s[t + 2 * j] * (h / 2 - 8));
        if (j) gfx_line(c, px, py, x + 8 + j, yy, UI_ACCENT);
        px = x + 8 + j; py = yy;
    }
}

static void slider(struct ui *ui, int x, int y, const char *name, int *v, int lo, int hi, const char *unit) {
    char b[24];
    ui_label(ui, x, y, name, UI_TEXT_DIM);
    ui_slider(ui, x + 8, y + 24, 84, v, lo, hi);
    snprintf(b, sizeof b, "%d %s", *v, unit);
    ui_label(ui, x, y + 46, b, UI_TEXT);
}

int main(void) {
    int fd = audio_open("piano");
    if (fd < 0) return 1;
    ioctl(fd, AUDIO_GETVOL, &vol);
    for (int i = 0; i < 1024; i++) sinetab[i] = sin(2 * M_PI * i / 1024);
    struct gui_window *w = gui_window_create("Piano", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("piano"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    static short out[CH * 2];
    static const char *const waves[] = { "Sine", "Square", "Saw", "Triangle" };
    bool mdown = false, was_active = false;
    int mnote = -1;
    uint32_t last = 0;
    do {
        /* ---- input ---- */
        for (int i = 0; i < ui.nkeys; i++) {
            struct gui_event *e = &ui.keys[i];
            if (e->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) continue;
            if (e->key == 'z' && oct > 1) oct--;
            else if (e->key == 'x' && oct < 6) oct++;
            else if (e->key > 0 && e->key < 128 && strchr(KEYS, e->key)) note_on(base() + (int)(strchr(KEYS, e->key) - KEYS), e->key);
        }
        for (int i = 0; i < ui.nups; i++) note_off(ui.ups[i]);
        if (!ui.focused) for (int i = 0; i < NV; i++) if (vo[i].src > 0) note_off(vo[i].src);
        if (ui.pressed && !mdown && (mnote = key_at(ui.mx, ui.my)) >= 0) { mdown = true; note_on(mnote, -1); }
        else if (mdown) {
            int n = (ui.buttons & 1) && !ui.released ? key_at(ui.mx, ui.my) : -1;
            if (n != mnote) {                                  /* released, left the keys, or glissando */
                note_off(-1);
                mnote = n;
                if (n >= 0) note_on(n, -1); else mdown = false;
            }
        }
        /* ---- audio ---- */
        for (int n = 0; n < 4 && any_voice() && audio_queued(fd) < QTARGET; n++) {
            gen(out);
            write(fd, out, sizeof out);          /* ring has room: queued < target */
        }
        bool active = any_voice();
        if (was_active && !active) memset(scope, 0, sizeof scope);
        /* ---- draw ---- */
        uint32_t now = uptime_ms();
        if (!ui.timeout || active || was_active) {
            if (ui.timeout && now - last < 30) goto next;
            last = now;
            ui_clear(&ui);
            ui_label(&ui, 22, 20, "Piano", UI_TEXT);
            ui_tabs(&ui, 90, 14, 300, 30, waves, 4, &wave_sel);
            ui_label(&ui, 420, 20, "Octave", UI_TEXT_DIM);
            if (ui_button(&ui, 486, 14, 32, 30, "-", UI_NORMAL_BTN) && oct > 1) oct--;
            char b[16];
            snprintf(b, sizeof b, "C%d", oct);
            gfx_text_center(ui.c, &font_ui_bold, 518, 20, 36, b, UI_TEXT);
            if (ui_button(&ui, 554, 14, 32, 30, "+", UI_NORMAL_BTN) && oct < 6) oct++;
            ui_label(&ui, 610, 20, "Vol", UI_TEXT_DIM);
            if (ui_slider(&ui, 646, 19, 84, &vol, 0, 100)) ioctl(fd, AUDIO_SETVOL, &vol);
            ui_panel(&ui, 20, 60, 440, 108);
            slider(&ui, 38, 70, "Attack", &A, 1, 500, "ms");
            slider(&ui, 138, 70, "Decay", &D, 1, 1000, "ms");
            slider(&ui, 238, 70, "Sustain", &S, 0, 100, "%");
            slider(&ui, 338, 70, "Release", &R, 5, 1500, "ms");
            draw_scope(&ui, 476, 60, 264, 108);
            draw_keys(&ui);
            ui_label(&ui, 30, KY + KH + 14, "Keys: A W S E D F T G Y H U J K O L P ;     Z / X: octave down / up     mouse: click or drag across the keys", UI_TEXT_DIM);
            ui_end(&ui);
        }
    next:
        was_active = active;
    } while (ui_poll(&ui, any_voice() ? 4 : 250));
    gui_window_destroy(w);
    close(fd);
    return 0;
}
