/* music.c - GENMIDI-driven OPL3 synth, MUS and SMF parsers, sequencer. See music.h.
 * The chip runs in OPL3 mode (18 two-operator voices, left/right per voice); each Doom instrument
 * takes one voice, or two when GENMIDI marks it double-voice, like DMX did on an OPL2. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "music.h"
#include "opl3.h"

#define NVOICE 18
#define NINSTR 175
#define GM_SIZE (8 + NINSTR * 36)

struct ev { uint32_t at, d; };                       /* d = status | data1 << 8 | data2 << 16 */
struct song { struct ev *e; int n; uint32_t total; int vol0; };

static opl3_chip chip;
static uint8_t gm[GM_SIZE];
static uint16_t fnum[128 * 16];                      /* block << 10 | fnum per 1/16 semitone */
static struct song *cur;
static int ei, loop, paused, playing, gain = 127;
static uint32_t pos, tail;

static struct {
    uint8_t prog, vol, expr, pan, sus;
    int bend;                                        /* -8192..8191 */
} ch[16];
/* sub = which of the instrument's two voices; f = last frequency word written */
static struct { int8_t chan; uint8_t note, vel, on, sus, sub; uint16_t f; uint32_t age; } vo[NVOICE];
static uint32_t tick;

/* ---- chip ---- */

static void wr(int c, int reg, int v) { OPL3_WriteReg(&chip, (c >= 9 ? 0x100 : 0) + reg, v); }
static const uint8_t opoff[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static void reset_state(int vol0) {
    OPL3_Reset(&chip, MUS_RATE);
    OPL3_WriteReg(&chip, 0x105, 1);                   /* OPL3 mode */
    OPL3_WriteReg(&chip, 0x01, 0x20);                 /* waveform select */
    memset(ch, 0, sizeof ch);
    for (int i = 0; i < 16; i++) { ch[i].vol = vol0; ch[i].expr = 127; ch[i].pan = 64; }
    memset(vo, 0, sizeof vo);
    for (int i = 0; i < NVOICE; i++) vo[i].chan = -1;
}

int mus_init(void) {
    if (!fnum[1]) {
        for (int i = 0; i < 128 * 16; i++) {
            double f = 440.0 * pow(2.0, (i / 16.0 - 69.0) / 12.0);
            int b = 0, fn = 0;
            for (; b < 8; b++) { fn = (int)(f * (1 << (20 - b)) / 49716.0 + 0.5); if (fn < 1024) break; }
            if (b == 8) { b = 7; fn = 1023; }
            fnum[i] = b << 10 | fn;
        }
    }
    reset_state(100);
    return 0;
}

int mus_genmidi(const uint8_t *d, size_t n) {
    if (n < GM_SIZE || memcmp(d, "#OPL_II#", 8)) return -1;
    memcpy(gm, d, GM_SIZE);
    return 0;
}

/* ---- voices ----
 * GENMIDI instrument (36 bytes): flags u16 (1 fixed pitch, 4 double voice), fine tuning, fixed note,
 * then two 16-byte voices: modulator {tremolo attack sustain wave scale level}, feedback/connection,
 * carrier {same six}, unused, base note s16. */

static const uint8_t *ivoice(int chan, int key, int sub, int *fixed, int *ft, int *dbl) {
    int i = chan == 9 ? (key >= 35 && key <= 81 ? 128 + key - 35 : -1) : ch[chan].prog;
    if (i < 0) return NULL;
    const uint8_t *p = gm + 8 + 36 * i;
    int fl = p[0] | p[1] << 8;
    *fixed = fl & 1 ? p[3] : -1;
    *ft = p[2];
    *dbl = fl & 4;
    return p + 4 + 16 * sub;
}

/* attenuation 0..63 for the carrier: instrument level lowered by velocity, channel volume, expression */
static int level_for(int chan, int vel, int base) {
    static uint8_t curve[128];
    if (!curve[127]) for (int i = 0; i < 128; i++) { int r = 0; while ((r + 1) * (r + 1) <= i * 127) r++; curve[i] = r; }
    int v = curve[vel] * ch[chan].vol / 127 * ch[chan].expr / 127;
    return 63 - ((63 - (base & 63)) * v >> 7);
}

static void voice_freq(int c, int on) {
    int fixed, ft, dbl, chn = vo[c].chan;
    const uint8_t *v = ivoice(chn, vo[c].note, vo[c].sub, &fixed, &ft, &dbl);
    if (v) {
        int n = fixed >= 0 ? fixed : vo[c].note;      /* GENMIDI base note offsets are not applied (they would drop everything an octave) */
        while (n < 0) n += 12;
        while (n > 119) n -= 12;
        int idx = (n << 4) + (ch[chn].bend >> 8);
        if (vo[c].sub) idx += (ft / 2 - 64) / 2;
        vo[c].f = fnum[idx < 0 ? 0 : idx > 128 * 16 - 1 ? 128 * 16 - 1 : idx];
    }
    wr(c, 0xA0 + c % 9, vo[c].f & 255);
    wr(c, 0xB0 + c % 9, (on ? 0x20 : 0) | (vo[c].f >> 10) << 2 | (vo[c].f >> 8 & 3));
}

static void voice_level(int c) {
    int fixed, ft, dbl;
    const uint8_t *v = ivoice(vo[c].chan, vo[c].note, vo[c].sub, &fixed, &ft, &dbl);
    if (!v) return;
    int o = opoff[c % 9];
    wr(c, 0x43 + o, (v[11] & 0xC0) | level_for(vo[c].chan, vo[c].vel, v[12]));
    if (v[6] & 1) wr(c, 0x40 + o, (v[4] & 0xC0) | level_for(vo[c].chan, vo[c].vel, v[5]));   /* additive: both are carriers */
}

static int pan_bits(int chan) { int p = ch[chan].pan; return p < 43 ? 0x10 : p > 85 ? 0x20 : 0x30; }

static void voice_off(int c) {
    if (vo[c].chan < 0 || !vo[c].on) return;
    vo[c].on = vo[c].sus = 0;
    voice_freq(c, 0);
}

static int alloc_voice(void) {                       /* a released voice (oldest), else the oldest sounding one */
    int best = 0, pri = 9;
    for (int i = 0; i < NVOICE; i++) {
        int p = vo[i].on ? 1 : 0;
        if (p < pri || (p == pri && vo[i].age < vo[best].age)) { best = i; pri = p; }
    }
    return best;
}

static void note_on(int chan, int key, int vel) {
    int fixed, ft, dbl;
    if (!ivoice(chan, key, 0, &fixed, &ft, &dbl)) return;
    for (int sub = 0; sub < (dbl ? 2 : 1); sub++) {
        int c = alloc_voice();
        const uint8_t *v = ivoice(chan, key, sub, &fixed, &ft, &dbl);
        int o = opoff[c % 9];
        wr(c, 0xB0 + c % 9, 0);                       /* key off so the envelope restarts */
        vo[c].chan = chan; vo[c].note = key; vo[c].vel = vel; vo[c].sub = sub; vo[c].sus = 0; vo[c].age = ++tick;
        wr(c, 0x20 + o, v[0]); wr(c, 0x60 + o, v[1]); wr(c, 0x80 + o, v[2]); wr(c, 0xE0 + o, v[3]);
        wr(c, 0x23 + o, v[7]); wr(c, 0x63 + o, v[8]); wr(c, 0x83 + o, v[9]); wr(c, 0xE3 + o, v[10]);
        wr(c, 0x40 + o, v[4] | v[5]);
        voice_level(c);
        wr(c, 0xC0 + c % 9, v[6] | pan_bits(chan));
        vo[c].on = 1;
        voice_freq(c, 1);
    }
}

static void note_off(int chan, int key) {
    for (int c = 0; c < NVOICE; c++)
        if (vo[c].on && vo[c].chan == chan && vo[c].note == key) {
            if (ch[chan].sus) vo[c].sus = 1; else voice_off(c);
        }
}

static void all_off(int chan) {
    for (int c = 0; c < NVOICE; c++)
        if (vo[c].on && vo[c].chan == chan) voice_off(c);
}

static void control(int chan, int n, int v) {
    switch (n) {
    case 7: ch[chan].vol = v; break;
    case 10: ch[chan].pan = v; break;
    case 11: ch[chan].expr = v; break;
    case 64:
        ch[chan].sus = v >= 64;
        if (!ch[chan].sus) for (int c = 0; c < NVOICE; c++) if (vo[c].sus && vo[c].chan == chan) voice_off(c);
        return;
    case 120: case 123: all_off(chan); return;
    case 121: ch[chan].expr = 127; ch[chan].bend = 0; ch[chan].sus = 0; return;
    default: return;
    }
    for (int c = 0; c < NVOICE; c++)
        if (vo[c].on && vo[c].chan == chan) {
            voice_level(c);
            if (n == 10) {
                int fixed, ft, dbl;
                const uint8_t *iv = ivoice(chan, vo[c].note, vo[c].sub, &fixed, &ft, &dbl);
                if (iv) wr(c, 0xC0 + c % 9, iv[6] | pan_bits(chan));
            }
        }
}

static void dispatch(uint32_t d) {
    int st = d & 255, a = d >> 8 & 127, b = d >> 16 & 127, chan = st & 15;
    switch (st >> 4) {
    case 8: note_off(chan, a); break;
    case 9: if (b) note_on(chan, a, b); else note_off(chan, a); break;
    case 11: control(chan, a, b); break;
    case 12: ch[chan].prog = a < NINSTR ? a : 0; break;
    case 14:
        ch[chan].bend = (a | b << 7) - 8192;
        for (int c = 0; c < NVOICE; c++) if (vo[c].on && vo[c].chan == chan) voice_freq(c, 1);
        break;
    }
}

/* ---- parsers ---- */

static int add(struct song *s, int *cap, uint32_t at, uint32_t d) {
    if (s->n == *cap) {
        struct ev *t = realloc(s->e, (*cap = *cap ? *cap * 2 : 4096) * sizeof *t);
        if (!t) return -1;
        s->e = t;
    }
    s->e[s->n].at = at; s->e[s->n++].d = d;
    return 0;
}

void mus_free(struct song *s) { if (s) { free(s->e); free(s); } }
uint32_t mus_frames(const struct song *s) { return s ? s->total : 0; }

/* MUS: 140 Hz ticks; events are 1 byte (last | type | channel) + args, a delay follows when `last` is set */
struct song *mus_parse_mus(const uint8_t *p, size_t n) {
    if (n < 16 || memcmp(p, "MUS\x1a", 4)) return NULL;
    size_t i = p[6] | p[7] << 8;
    struct song *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->vol0 = 127;
    static const uint8_t ctl[10] = { 0, 0, 1, 7, 10, 11, 91, 93, 64, 67 };   /* MUS controller -> MIDI CC */
    uint8_t lastvel[16] = { 0 };
    uint32_t t = 0;
    int cap = 0, done = 0;
    while (!done && i < n) {
        int b = p[i++], last = b & 128, type = b >> 4 & 7, c = b & 15, m;
        c = c == 15 ? 9 : c >= 9 ? c + 1 : c;        /* MUS percussion is 15, MIDI's is 9 */
        uint32_t at = (uint32_t)((uint64_t)t * MUS_RATE / 140), d = 0;
        switch (type) {
        case 0: if (i >= n) goto bad; d = 0x80 | c | p[i++] << 8; break;
        case 1:
            if (i >= n) goto bad;
            m = p[i++];
            if (m & 128) { if (i >= n) goto bad; lastvel[c] = p[i++] & 127; }
            d = 0x90 | c | (m & 127) << 8 | lastvel[c] << 16;
            break;
        case 2: if (i >= n) goto bad; m = p[i++] << 6; d = 0xE0 | c | (m & 127) << 8 | (m >> 7) << 16; break;
        case 3:
            if (i >= n) goto bad;
            m = p[i++];
            if (m == 10 || m == 11 || m == 14) d = 0xB0 | c | (m == 10 ? 120 : m == 11 ? 123 : 121) << 8;
            break;
        case 4:
            if (i + 1 >= n) goto bad;
            m = p[i++];
            if (m == 0) d = 0xC0 | c | (p[i] & 127) << 8;
            else if (m < 10) d = 0xB0 | c | ctl[m] << 8 | (p[i] & 127) << 16;
            i++;
            break;
        case 6: done = 1; break;
        default: break;
        }
        if (d && add(s, &cap, at, d) < 0) goto bad;
        if (last && !done) {
            uint32_t dt = 0;
            do { if (i >= n) goto bad; dt = dt << 7 | (p[i] & 127); } while (p[i++] & 128);
            t += dt;
        }
    }
    s->total = (uint32_t)((uint64_t)t * MUS_RATE / 140);
    return s;
bad:
    mus_free(s);
    return NULL;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

struct tev { uint32_t tick, d, idx; };               /* d = 0xFF | tempo << 8 for a tempo change */

static int tadd(struct tev **te, int *nt, int *cap, uint32_t tick, uint32_t d) {
    if (*nt == *cap) {
        struct tev *x = realloc(*te, (*cap = *cap ? *cap * 2 : 4096) * sizeof *x);
        if (!x) return -1;
        *te = x;
    }
    (*te)[*nt].tick = tick; (*te)[*nt].d = d; (*te)[*nt].idx = *nt;
    (*nt)++;
    return 0;
}

static int tcmp(const void *a, const void *b) {
    const struct tev *x = a, *y = b;
    return x->tick != y->tick ? (x->tick < y->tick ? -1 : 1) : x->idx < y->idx ? -1 : x->idx > y->idx;
}

static int varlen(const uint8_t *p, size_t *q, size_t e, uint32_t *v) {
    *v = 0;
    do { if (*q >= e) return -1; *v = *v << 7 | (p[*q] & 127); } while (p[(*q)++] & 128);
    return 0;
}

/* SMF: tracks are merged by tick, then ticks become frames through the tempo map */
struct song *mus_parse_smf(const uint8_t *p, size_t n) {
    if (n < 14 || memcmp(p, "MThd", 4) || rd32(p + 4) < 6) return NULL;
    int ntrk = p[10] << 8 | p[11], div = p[12] << 8 | p[13];
    if (!div || div & 0x8000) return NULL;           /* SMPTE timing: not supported */
    size_t i = 8 + rd32(p + 4);
    struct tev *te = NULL;
    int nt = 0, cap = 0;
    struct song *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->vol0 = 100;
    for (int tr = 0; tr < ntrk && i + 8 <= n; tr++) {
        size_t len = rd32(p + i + 4), q = i + 8, e = q + len < n ? q + len : n;
        i = q + len;
        uint32_t t = 0, v;
        int run = 0;
        while (q < e) {
            if (varlen(p, &q, e, &v) < 0) break;
            t += v;
            if (q >= e) break;
            int st = p[q];
            if (st & 128) q++; else st = run;
            if (st == 0xFF) {
                if (q >= e) break;
                int mt = p[q++];
                if (varlen(p, &q, e, &v) < 0) break;
                if (mt == 0x51 && v == 3 && q + 3 <= e && (p[q] | p[q + 1] | p[q + 2]))
                    if (tadd(&te, &nt, &cap, t, 0xFF | (uint32_t)(p[q] << 16 | p[q + 1] << 8 | p[q + 2]) << 8) < 0) goto bad;
                q += v; run = 0;
                if (mt == 0x2F) break;
            } else if (st == 0xF0 || st == 0xF7) {   /* sysex */
                if (varlen(p, &q, e, &v) < 0) break;
                q += v; run = 0;
            } else {
                if (st < 0x80 || st >= 0xF0) break;
                run = st;
                int need = (st >> 4 == 12 || st >> 4 == 13) ? 1 : 2;
                if (q + need > e) break;
                uint32_t d = st | p[q] << 8 | (need == 2 ? p[q + 1] << 16 : 0);
                q += need;
                int k = st >> 4;
                if ((k == 8 || k == 9 || k == 11 || k == 12 || k == 14) && tadd(&te, &nt, &cap, t, d) < 0) goto bad;
            }
        }
    }
    qsort(te, nt, sizeof *te, tcmp);
    uint64_t tempo = 500000, base_f = 0;             /* us per quarter note; frames at base_t */
    uint32_t base_t = 0;
    int ecap = 0;
    for (int k = 0; k < nt; k++) {
        uint64_t f = base_f + (uint64_t)(te[k].tick - base_t) * tempo * (MUS_RATE / 1000) / ((uint64_t)div * 1000);
        if ((te[k].d & 255) == 0xFF) { base_f = f; base_t = te[k].tick; tempo = te[k].d >> 8; continue; }
        if (add(s, &ecap, (uint32_t)f, te[k].d) < 0) goto bad;
        s->total = (uint32_t)f;
    }
    free(te);
    return s;
bad:
    free(te);
    mus_free(s);
    return NULL;
}

/* ---- sequencer ---- */

void mus_volume(int v) { gain = v < 0 ? 0 : v > 127 ? 127 : v; }
int mus_playing(void) { return playing; }
void mus_pause(int on) { paused = on; }
uint32_t mus_pos(void) { return pos; }

void mus_stop(void) {
    if (cur) reset_state(cur->vol0);
    playing = 0; cur = NULL;
}

void mus_play(struct song *s, int lp) {
    mus_stop();
    if (!s || !s->n) return;
    reset_state(s->vol0);
    cur = s; ei = 0; pos = 0; loop = lp; playing = 1; tail = MUS_RATE / 2; paused = 0;
}

void mus_render(int *acc, int n) {
    int16_t b[2];
    if (!playing || paused) return;
    while (n > 0) {
        int k = n;
        if (ei < cur->n && cur->e[ei].at <= pos) { dispatch(cur->e[ei++].d); continue; }
        if (ei < cur->n) { if (cur->e[ei].at - pos < (uint32_t)k) k = cur->e[ei].at - pos; }
        else if (pos < cur->total) { if (cur->total - pos < (uint32_t)k) k = cur->total - pos; }
        else if (loop) { reset_state(cur->vol0); ei = 0; pos = 0; continue; }
        else {                                       /* let the last notes ring out for half a second */
            if (!tail) { mus_stop(); return; }
            if (tail < (uint32_t)k) k = tail;
            tail -= k;
        }
        for (int i = 0; i < k; i++) {
            OPL3_GenerateResampled(&chip, b);
            acc[2 * i] += b[0] * gain >> 5;      /* the chip is quiet: x4 (peaks ~-7 dBFS on dense songs) */
            acc[2 * i + 1] += b[1] * gain >> 5;
        }
        acc += 2 * k; pos += k; n -= k;
    }
}
