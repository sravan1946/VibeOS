/* i_vibesound.c - doomgeneric sound module for /dev/dsp (48 kHz stereo s16le).
 * SFX lumps (8-bit mono, mostly 11025 Hz) are mixed in 8 ms chunks with linear resampling; the
 * mixer only runs while a sound is playing and keeps ~40 ms queued (polled via AUDIO_GETQ), so
 * the game loop never blocks in write(). Music: MUS lumps are turned into MIDI events and played by the
 * OPL3 synth in user/music/ (GENMIDI instruments); it is rendered into the same 8 ms mix chunks. */
#include <audio.h>
#include <stdlib.h>
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "deh_str.h"
#include "z_zone.h"
#include "music.h"

#define NCH 16
#define CHUNK 384                               /* frames per write: 8 ms */
#define TARGET (AUDIO_RATE / 25 * 4)            /* bytes to keep queued: 40 ms */

struct sample { uint32_t n, rate; uint8_t d[]; };   /* unsigned 8-bit */
static struct { const struct sample *s; uint32_t pos, step; int l, r; } ch[NCH];
static int fd = -1, prefix;
void vibe_sound_pump(void);

/* other code (m_config.c) wants these when FEATURE_SOUND is on */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* Doom's panning: sep 0 (left) .. 254 (right), 128 = centre */
static void set_vol(int c, int vol, int sep) {
    ch[c].l = vol - vol * sep * sep / 65536;
    ch[c].r = vol - vol * (sep - 256) * (sep - 256) / 65536;
}

static void lump_name(sfxinfo_t *sfx, char *buf, size_t n) {
    if (sfx->link) sfx = sfx->link;
    if (prefix) M_snprintf(buf, n, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(buf, DEH_String(sfx->name), n);
}

static int get_lump(sfxinfo_t *sfx) {
    char name[9];
    lump_name(sfx, name, sizeof name);
    return W_CheckNumForName(name);
}

/* DMX header: 03 00, u16 rate, u32 length; like DMX itself skip 16 bytes at each end */
static const struct sample *load(sfxinfo_t *sfx) {
    if (sfx->driver_data) return sfx->driver_data;
    if (sfx->lumpnum < 0) return NULL;
    uint8_t *p = W_CacheLumpNum(sfx->lumpnum, PU_CACHE);
    unsigned len = W_LumpLength(sfx->lumpnum);
    if (len < 8 || p[0] != 3 || p[1] != 0) return NULL;
    unsigned n = p[4] | p[5] << 8 | p[6] << 16 | (unsigned)p[7] << 24;
    if (n > len - 8 || n <= 48) return NULL;
    struct sample *s = malloc(sizeof *s + n - 32);
    if (!s) return NULL;
    s->n = n - 32;
    s->rate = p[2] | p[3] << 8;
    memcpy(s->d, p + 8 + 16, s->n);
    sfx->driver_data = s;
    return s;
}

static boolean snd_init(boolean use_prefix) {
    prefix = use_prefix;
    fd = open("/dev/dsp", O_WRONLY);
    return fd >= 0;                              /* no card: the game runs silent */
}
static void snd_shutdown(void) { if (fd >= 0) { close(fd); fd = -1; } }
static int snd_lump(sfxinfo_t *sfx) { return get_lump(sfx); }
static void snd_update(void) { vibe_sound_pump(); }
static void snd_params(int c, int vol, int sep) { if (c >= 0 && c < NCH && ch[c].s) set_vol(c, vol, sep); }

static int snd_start(sfxinfo_t *sfx, int c, int vol, int sep) {
    if (fd < 0 || c < 0 || c >= NCH) return -1;
    ch[c].s = NULL;
    const struct sample *s = load(sfx);
    if (!s) return -1;
    set_vol(c, vol, sep);
    ch[c].pos = 0;
    ch[c].step = (uint32_t)((uint64_t)s->rate * 65536 / AUDIO_RATE);
    ch[c].s = s;
    return c;
}
static void snd_stop(int c) { if (c >= 0 && c < NCH) ch[c].s = NULL; }
static boolean snd_playing(int c) { return c >= 0 && c < NCH && ch[c].s; }

static int mix(short *out) {
    static int acc[CHUNK * 2];
    int live = 0;
    memset(acc, 0, sizeof acc);
    for (int c = 0; c < NCH; c++) {
        const struct sample *s = ch[c].s;
        if (!s) continue;
        uint32_t pos = ch[c].pos;
        int i = 0;
        for (; i < CHUNK; i++, pos += ch[c].step) {
            uint32_t k = pos >> 16, f = (pos >> 8) & 255;
            if (k + 1 >= s->n) break;
            int v = ((s->d[k] - 128) * (int)(256 - f) + (s->d[k + 1] - 128) * (int)f);   /* 16-bit scale */
            acc[2 * i] += v * ch[c].l >> 8;
            acc[2 * i + 1] += v * ch[c].r >> 8;
        }
        if (i < CHUNK) ch[c].s = NULL; else { ch[c].pos = pos; live++; }
    }
    mus_render(acc, CHUNK);
    live += mus_playing();
    for (int i = 0; i < CHUNK * 2; i++) out[i] = acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i];
    return live;
}

/* called from the platform layer and every tic: top the ring up while anything is playing */
void vibe_sound_pump(void) {
    static short out[CHUNK * 2];
    int any = 0;
    if (fd < 0) return;
    for (int c = 0; c < NCH; c++) any |= ch[c].s != NULL;
    any |= mus_playing();
    for (int q = audio_queued(fd); any && q < TARGET; q += CHUNK * 4) {
        any = mix(out) > 0;
        write(fd, out, sizeof out);              /* ring has room: queued < target */
    }
}
void vibe_sound_close(void) { snd_shutdown(); }   /* drains what is queued */

static snddevice_t devs[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    devs, sizeof devs / sizeof *devs, snd_init, snd_shutdown, snd_lump, snd_update,
    snd_params, snd_start, snd_stop, snd_playing, NULL,
};

/* ---- music: MUS (or MIDI) lump -> OPL3 synth; GENMIDI comes from the WAD ---- */
static int music_up;
static boolean mm_init(void) {
    int l = W_CheckNumForName("GENMIDI");
    if (fd < 0 || l < 0 || mus_init() < 0) return false;
    music_up = mus_genmidi(W_CacheLumpNum(l, PU_STATIC), W_LumpLength(l)) == 0;
    return music_up;
}
static void mm_shutdown(void) { mus_stop(); }
static void mm_vol(int v) { mus_volume(v); }
static void mm_pause(void) { mus_pause(1); }
static void mm_resume(void) { mus_pause(0); }
static void *mm_reg(void *d, int n) {
    if (!music_up) return NULL;
    struct song *s = mus_parse_mus(d, n);
    return s ? s : mus_parse_smf(d, n);
}
static void mm_unreg(void *h) { mus_stop(); mus_free(h); }
static void mm_play(void *h, boolean loop) { if (h) mus_play(h, loop); }
static void mm_stop(void) { mus_stop(); }
static boolean mm_playing(void) { return mus_playing(); }

music_module_t DG_music_module = {
    devs, sizeof devs / sizeof *devs, mm_init, mm_shutdown, mm_vol, mm_pause, mm_resume,
    mm_reg, mm_unreg, mm_play, mm_stop, mm_playing, NULL,
};
