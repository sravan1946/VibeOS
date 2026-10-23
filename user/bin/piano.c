/* piano - two-octave on-screen keyboard (mouse or A W S E D F T G Y H U J K O L P ;) with a polyphonic
 * synth: sine/square/saw/triangle, ADSR envelope, scope. Audio is generated in 10 ms chunks and written to
 * /dev/dsp only while fewer than ~30 ms are queued, so the UI never blocks on the sound card and latency stays low.
 * ponytail: naive (aliasing) square/saw, no oscillator interpolation; fine below ~2 kHz. */
#include "game.h"
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
#define KY (HEAD + SP_5 + 108 + SP_5)
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

/* key content colours: ivory and graphite in every theme; a held key takes the accent (it is state) */
static void draw_keys(struct ui *ui) {
    struct canvas *c = ui->c;
    color_t ivory = RGB(0xEE, 0xEC, 0xE7), ebony = RGB(0x2A, 0x2B, 0x2F);
    for (int pass = 0; pass < 2; pass++)                      /* whites, then blacks on top */
        for (int s = 0; s < NKEYS; s++) {
            int blk = BLACK[s % 12];
            if (blk != pass) continue;
            int x = keyx(s), w = blk ? BW : KW - 2, h = blk ? BH : KH;
            bool on = held(base() + s);
            color_t col = blk ? (on ? UI_ACCENT : ebony) : (on ? mix(ivory, UI_ACCENT, 140) : ivory);
            gfx_round_fill(c, x, KY, w, h, R_2, col);
            gfx_round_rect(c, x, KY, w, h, R_2, blk ? RGBA(0, 0, 0, 120) : RGBA(0, 0, 0, 40));
            if (s < 17) {
                char l[2] = { KEYS[s] == ';' ? ';' : KEYS[s] - 32, 0 };
                gfx_text_center(c, T_CAPTION_B, x, KY + h - SP_7, w, l, blk ? (on ? UI_ON_ACCENT : RGB(0xB4, 0xB6, 0xBB)) : RGB(0x5A, 0x5C, 0x62));
            }
            if (!blk && s % 12 == 0) {
                char n[8];
                snprintf(n, sizeof n, "C%d", oct + s / 12);
                gfx_text_center(c, T_CAPTION, x, KY + h - SP_7 - 20, w, n, RGB(0x6E, 0x70, 0x76));
            }
        }
}

static void draw_scope(struct ui *ui, int x, int y, int w, int h, bool active) {
    struct canvas *c = ui->c;
    ui_card(ui, x, y, w, h);
    int cy = y + h / 2;
    gfx_fill(c, x + SP_3, cy, w - 2 * SP_3, 1, UI_LINE);
    if (!active) return;
    float s[1600];
    for (int i = 0; i < 1600; i++) s[i] = scope[(scope_pos - 1600 + i) & 2047];
    int t = 1;
    while (t < 600 && !(s[t - 1] < 0 && s[t] >= 0)) t++;      /* trigger on a rising zero crossing */
    if (t >= 600) t = 0;
    int px = x + SP_3, py = cy;
    for (int j = 0; j < w - 2 * SP_3 && t + 2 * j < 1600; j++) {
        int yy = cy - (int)(s[t + 2 * j] * (h / 2 - SP_3));
        if (j) gfx_line_aa(c, px, py, x + SP_3 + j, yy, 1.5f, UI_ACCENT_TEXT);
        px = x + SP_3 + j; py = yy;
    }
}

/* one envelope column in the card: name and value on a row, the slider below */
static void slider(struct ui *ui, int x, int y, int w, const char *name, int *v, int lo, int hi, const char *unit) {
    char b[24];
    gfx_text(ui->c, T_BODY, x, y, name, UI_TEXT);
    snprintf(b, sizeof b, "%d %s", *v, unit);
    gfx_text(ui->c, T_BODY, x + w - gfx_text_width(T_BODY, b), y, b, UI_TEXT2);
    ui_slider(ui, x, y + T_BODY->height + SP_3, w, v, lo, hi);
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
            game_head(&ui, W);
            ui_segmented(&ui, SP_5, CTL_Y, NULL, waves, 4, &wave_sel);
            int rx = W - SP_5;
            rx -= 100;
            if (ui_slider(&ui, rx, CTL_Y + 2, 100, &vol, 0, 100)) ioctl(fd, AUDIO_SETVOL, &vol);
            rx -= SP_3 + ICON_SM;
            ui_icon(&ui, rx, (HEAD - ICON_SM) / 2, vol ? ICON_VOLUME_HIGH : ICON_VOLUME_MUTE, ICON_SM, UI_TEXT2);
            rx -= SP_6;
            if (game_btn(&ui, &rx, ICON_PLUS, NULL, UI_GHOST | (oct < 6 ? 0 : UI_DISABLED)) && oct < 6) oct++;
            ui_tooltip(&ui, rx + SP_3, CTL_Y, H_CTL, H_CTL, "Octave up · X");
            char b[16];
            snprintf(b, sizeof b, "C%d", oct);
            rx -= 28;
            gfx_text_center(ui.c, T_BODY_B, rx, (HEAD - T_BODY_B->height) / 2, 28, b, UI_TEXT);
            rx -= SP_2;
            if (game_btn(&ui, &rx, ICON_MINUS, NULL, UI_GHOST | (oct > 1 ? 0 : UI_DISABLED)) && oct > 1) oct--;
            ui_tooltip(&ui, rx + SP_3, CTL_Y, H_CTL, H_CTL, "Octave down · Z");
            gfx_text(ui.c, T_BODY, rx + SP_3 - SP_3 - gfx_text_width(T_BODY, "Octave"), (HEAD - T_BODY->height) / 2, "Octave", UI_TEXT2);
            int cy = HEAD + SP_5, cw4 = 500;
            ui_card(&ui, SP_5, cy, cw4, 108);
            gfx_text(ui.c, T_CAPTION_B, SP_5 + SP_4, cy + SP_4, "Envelope", UI_TEXT2);
            int colw = (cw4 - 2 * SP_4 - 3 * SP_5) / 4;
            slider(&ui, SP_5 + SP_4, cy + 40, colw, "Attack", &A, 1, 500, "ms");
            slider(&ui, SP_5 + SP_4 + (colw + SP_5), cy + 40, colw, "Decay", &D, 1, 1000, "ms");
            slider(&ui, SP_5 + SP_4 + 2 * (colw + SP_5), cy + 40, colw, "Sustain", &S, 0, 100, "%");
            slider(&ui, SP_5 + SP_4 + 3 * (colw + SP_5), cy + 40, colw, "Release", &R, 5, 1500, "ms");
            draw_scope(&ui, SP_5 + cw4 + SP_5, cy, W - 3 * SP_5 - cw4, 108, active || was_active);
            draw_keys(&ui);
            gfx_text(ui.c, T_CAPTION, KX, KY + KH + SP_4, "Play with A to ; on the keyboard or click and drag across the keys. Z and X change the octave.", UI_TEXT2);
            ui_end(&ui);
        }
    next:
        was_active = active;
    } while (ui_poll(&ui, any_voice() ? 4 : 250));
    gui_window_destroy(w);
    close(fd);
    return 0;
}
