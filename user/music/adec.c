/* adec.c - WAV / MP3 / Ogg Vorbis -> 48 kHz stereo s16le. Each format fills `sbuf` with native-rate stereo frames
 * (src_refill); a linear resampler turns those into ADEC_RATE frames. MP3 and Ogg are read into memory whole
 * (a few MB) so seeking is cheap: the MP3 frame table is built at open, Ogg seeks through stb_vorbis.
 * ponytail: linear interpolation (no low-pass) when resampling, ID3v2.3/2.4 only, no unsynchronisation,
 * the encoder delay (~1105 samples) is played rather than trimmed. */
#include "adec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "minimp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

enum { K_WAV, K_MP3, K_OGG };
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SB 2304                                 /* native frames in sbuf: room for one MP3 frame (1152) or an Ogg slice */

struct adec {
    int kind, rate, chans, len_ms;
    char info[64];
    int16_t sbuf[SB * 2];
    int spos, slen;                             /* next native frame, frames in sbuf */
    uint32_t acc;                               /* resampler phase: position between source frames, in 1/ADEC_RATE */
    int al, ar, bl, br, primed;                 /* interpolation pair */
    FILE *f;                                    /* WAV */
    int bits, left;                             /* WAV: sample size, data bytes not yet read */
    long data_at, data_len;
    uint8_t *file; size_t size;                 /* MP3 / Ogg: the whole file */
    mp3dec_t dec;                               /* MP3 */
    uint32_t *fofs; int nf, cur, spf;           /* MP3: frame offsets, next frame, samples per frame */
    stb_vorbis *v;                              /* Ogg */
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- ID3v2 ---- */
static uint32_t syncsafe(const uint8_t *p) { return (p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

/* an ID3 text field as UTF-8 into out[cap] */
static void id3_text(char *out, int cap, const uint8_t *p, uint32_t n) {
    int enc = n ? p[0] : 0, o = 0;
    p++; n = n ? n - 1 : 0;
    if (enc == 1 || enc == 2) {                 /* UTF-16 (BOM or big endian) */
        int be = enc == 2;
        if (enc == 1 && n >= 2) { be = p[0] == 0xFE; p += 2; n -= 2; }
        for (uint32_t i = 0; i + 1 < n && o < cap - 4; i += 2) {
            uint32_t c = be ? p[i] << 8 | p[i + 1] : p[i + 1] << 8 | p[i];
            if (!c) break;
            if (c < 0x80) out[o++] = c;
            else if (c < 0x800) { out[o++] = 0xC0 | c >> 6; out[o++] = 0x80 | (c & 63); }
            else if (c >= 0xD800 && c < 0xE000) out[o++] = '?';
            else { out[o++] = 0xE0 | c >> 12; out[o++] = 0x80 | (c >> 6 & 63); out[o++] = 0x80 | (c & 63); }
        }
    } else if (enc == 0) {                      /* Latin-1 */
        for (uint32_t i = 0; i < n && p[i] && o < cap - 2; i++) {
            if (p[i] < 0x80) out[o++] = p[i];
            else { out[o++] = 0xC0 | p[i] >> 6; out[o++] = 0x80 | (p[i] & 63); }
        }
    } else {                                    /* UTF-8 */
        for (uint32_t i = 0; i < n && p[i] && o < cap - 1; i++) out[o++] = p[i];
    }
    out[o] = 0;
}

/* bytes of ID3v2 tag at the start of the file (0 if none); fills tags from the v2.3/2.4 frames */
static size_t id3_parse(const uint8_t *d, size_t size, struct adec_tags *t) {
    if (size < 10 || memcmp(d, "ID3", 3) || d[3] < 3 || d[3] > 4 || (d[5] & 0x40)) return size >= 10 && !memcmp(d, "ID3", 3) ? 10 + syncsafe(d + 6) : 0;
    size_t end = 10 + syncsafe(d + 6);
    if (end > size) end = size;
    size_t o = 10;
    while (o + 10 <= end && d[o]) {
        uint32_t n = d[3] == 4 ? syncsafe(d + o + 4) : (uint32_t)d[o + 4] << 24 | d[o + 5] << 16 | d[o + 6] << 8 | d[o + 7];
        const uint8_t *p = d + o + 10;
        if (n > end - o - 10) break;
        if (t) {
            if (!memcmp(d + o, "TIT2", 4)) id3_text(t->title, sizeof t->title, p, n);
            else if (!memcmp(d + o, "TPE1", 4)) id3_text(t->artist, sizeof t->artist, p, n);
            else if (!memcmp(d + o, "TALB", 4)) id3_text(t->album, sizeof t->album, p, n);
            else if (!memcmp(d + o, "APIC", 4) && !t->art && n > 4) {
                /* enc, mime\0, type, description\0 (double NUL for UTF-16), picture bytes */
                uint32_t i = 1;
                while (i < n && p[i]) i++;
                i += 2;                         /* mime's NUL + picture type */
                if (p[0] == 1 || p[0] == 2) { while (i + 1 < n && (p[i] || p[i + 1])) i += 2; i += 2; }
                else { while (i < n && p[i]) i++; i++; }
                if (i < n) { t->art = p + i; t->art_len = n - i; }
            }
        }
        o += 10 + n;
    }
    return end;
}

/* ---- sources: refill sbuf with native stereo frames, 0 at the end ---- */
static int wav_refill(struct adec *d) {
    int fb = d->chans * d->bits / 8, want = d->left < SB * fb ? d->left : SB * fb;
    static uint8_t raw[SB * 4];
    want -= want % fb;
    int got = want ? (int)fread(raw, 1, want, d->f) / fb : 0;
    d->left -= got * fb;
    for (int i = 0; i < got; i++) {
        const uint8_t *p = raw + i * fb;
        int l, r;
        if (d->bits == 8) { l = (p[0] - 128) * 256; r = d->chans == 2 ? (p[1] - 128) * 256 : l; }
        else { l = (int16_t)(p[0] | p[1] << 8); r = d->chans == 2 ? (int16_t)(p[2] | p[3] << 8) : l; }
        d->sbuf[2 * i] = l; d->sbuf[2 * i + 1] = r;
    }
    d->spos = 0; d->slen = got;
    return got;
}

/* decode frame idx into sbuf; a frame the decoder can't use yet (no bit reservoir after a seek, the Xing/Info frame)
 * is silence, which keeps the timing */
static int mp3_frame(struct adec *d, int idx) {
    static int16_t pcm[1152 * 2];
    mp3dec_frame_info_t fi;
    uint32_t at = d->fofs[idx];
    int n = mp3dec_decode_frame(&d->dec, d->file + at, (int)(d->size - at), pcm, &fi);
    if (n <= 0) { memset(d->sbuf, 0, d->spf * 4); n = d->spf; }
    else for (int i = 0; i < n; i++) {
        d->sbuf[2 * i] = pcm[i * fi.channels];
        d->sbuf[2 * i + 1] = pcm[i * fi.channels + (fi.channels - 1)];
    }
    d->spos = 0; d->slen = n;
    return n;
}

static int mp3_refill(struct adec *d) { return d->cur < d->nf ? mp3_frame(d, d->cur++) : 0; }

static int ogg_refill(struct adec *d) {
    int n = stb_vorbis_get_samples_short_interleaved(d->v, 2, d->sbuf, 2048 * 2);
    d->spos = 0; d->slen = n;
    return n;
}

static int src_refill(struct adec *d) { return d->kind == K_WAV ? wav_refill(d) : d->kind == K_MP3 ? mp3_refill(d) : ogg_refill(d); }

static int next_frame(struct adec *d, int *l, int *r) {
    if (d->spos >= d->slen && !src_refill(d)) return 0;
    *l = d->sbuf[2 * d->spos]; *r = d->sbuf[2 * d->spos + 1];
    d->spos++;
    return 1;
}

int adec_read(struct adec *d, int16_t *out, int frames) {
    int n = 0;
    if (!d->primed) {
        d->primed = 1;
        if (!next_frame(d, &d->al, &d->ar) || !next_frame(d, &d->bl, &d->br)) return 0;
    }
    while (n < frames) {
        if (d->al == 0x10000) break;            /* ended */
        int fr = (int)(d->acc * 65536u / ADEC_RATE);
        out[2 * n] = d->al + (int)(((int64_t)(d->bl - d->al) * fr) >> 16);
        out[2 * n + 1] = d->ar + (int)(((int64_t)(d->br - d->ar) * fr) >> 16);
        n++;
        d->acc += d->rate;                      /* exact: no drift from a rounded 16.16 step */
        for (; d->acc >= ADEC_RATE; d->acc -= ADEC_RATE) {
            d->al = d->bl; d->ar = d->br;
            if (!next_frame(d, &d->bl, &d->br)) { d->al = 0x10000; break; }
        }
    }
    return n;
}

int adec_length_ms(struct adec *d) { return d->len_ms; }
const char *adec_info(struct adec *d) { return d->info; }

int adec_seek(struct adec *d, int ms) {
    if (ms < 0) ms = 0;
    d->spos = d->slen = 0; d->acc = 0; d->primed = 0;
    if (d->kind == K_WAV) {
        long fb = d->chans * d->bits / 8, tot = d->data_len / fb, fr = (long)((int64_t)ms * d->rate / 1000);
        if (fr > tot) fr = tot;
        if (fseek(d->f, d->data_at + fr * fb, SEEK_SET)) return -1;
        d->left = (int)((tot - fr) * fb);
    } else if (d->kind == K_MP3) {
        int64_t at = (int64_t)ms * d->rate / 1000;
        int k = (int)(at / d->spf), from = k > 2 ? k - 2 : 0;       /* two frames of lead-in refill the bit reservoir */
        if (k > d->nf) k = d->nf;
        mp3dec_init(&d->dec);
        for (int i = from; i < k; i++) mp3_frame(d, i);
        d->spos = d->slen = 0; d->cur = k;
        if (mp3_refill(d)) d->spos = MIN((int)(at - (int64_t)k * d->spf), d->slen);
    } else if (!stb_vorbis_seek(d->v, (unsigned)((int64_t)ms * d->rate / 1000))) return -1;
    return 0;
}

void adec_close(struct adec *d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    if (d->v) stb_vorbis_close(d->v);
    free(d->file); free(d->fofs);
    free(d);
}

int adec_is_audio(const char *name) {
    const char *e = strrchr(name, '.');
    return e && (!strcasecmp(e, ".wav") || !strcasecmp(e, ".mp3") || !strcasecmp(e, ".ogg"));
}

/* ---- open ---- */
static int open_wav(struct adec *d, FILE *f, const char **err) {
    uint8_t h[12], ch[8];
    int tag = 0, ok = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { *err = "not a WAV, MP3 or Ogg file"; return -1; }
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t n = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4) && n >= 16) {
            uint8_t fm[40];
            uint32_t k = n < sizeof fm ? n : sizeof fm;
            if (fread(fm, 1, k, f) != k) break;
            tag = fm[0] | fm[1] << 8; d->chans = fm[2] | fm[3] << 8; d->rate = le32(fm + 4); d->bits = fm[14] | fm[15] << 8;
            if (tag == 0xFFFE && k >= 26) tag = fm[24] | fm[25] << 8;      /* WAVE_FORMAT_EXTENSIBLE sub-format */
            fseek(f, n - k + (n & 1), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) { d->left = n; ok = 1; break; }
        else fseek(f, n + (n & 1), SEEK_CUR);
    }
    if (!ok || tag != 1 || (d->bits != 8 && d->bits != 16) || d->chans < 1 || d->chans > 2 || d->rate < 1000 || d->rate > 192000) {
        *err = "unsupported WAV (need PCM 8/16-bit, 1-2 channels)"; return -1;
    }
    d->f = f; d->data_at = ftell(f); d->data_len = d->left;
    d->kind = K_WAV;
    d->len_ms = (int)((int64_t)d->left / (d->chans * d->bits / 8) * 1000 / d->rate);
    snprintf(d->info, sizeof d->info, "WAV, %d Hz, %d-bit, %s", d->rate, d->bits, d->chans == 2 ? "stereo" : "mono");
    return 0;
}

static int open_mp3(struct adec *d, struct adec_tags *t, const char **err) {
    size_t off = id3_parse(d->file, d->size, t), cap = 0;
    mp3dec_t scan;
    mp3dec_frame_info_t fi, first = { 0 };
    mp3dec_init(&scan);
    while (off < d->size) {
        int n = mp3dec_decode_frame(&scan, d->file + off, (int)(d->size - off), NULL, &fi);
        if (!fi.frame_bytes) break;
        if (n > 0) {
            if (!first.hz) { first = fi; d->spf = n; }
            if (fi.hz == first.hz) {            /* ignore frames of another stream (a stray tag, a rate change) */
                if (d->nf == (int)cap) {
                    cap = cap ? cap * 2 : 4096;
                    uint32_t *g = realloc(d->fofs, cap * 4);
                    if (!g) { *err = "out of memory"; return -1; }
                    d->fofs = g;
                }
                d->fofs[d->nf++] = off + fi.frame_offset;
            }
        }
        off += fi.frame_bytes;
    }
    if (!d->nf) { *err = "no MP3 frames found"; return -1; }
    for (int i = 4; i < 48 && d->fofs[0] + i + 4 <= d->size; i++)       /* a Xing/Info frame is a header, not audio */
        if (d->nf > 1 && (!memcmp(d->file + d->fofs[0] + i, "Xing", 4) || !memcmp(d->file + d->fofs[0] + i, "Info", 4))) {
            memmove(d->fofs, d->fofs + 1, --d->nf * 4);
            break;
        }
    d->kind = K_MP3; d->rate = first.hz; d->chans = first.channels;
    d->len_ms = (int)((int64_t)d->nf * d->spf * 1000 / d->rate);
    mp3dec_init(&d->dec);
    snprintf(d->info, sizeof d->info, "MP3, %d Hz, %s, %d kbps", d->rate, d->chans == 2 ? "stereo" : "mono", first.bitrate_kbps);
    return 0;
}

static void vorbis_tag(char *out, int cap, const char *c, const char *key) {
    int n = (int)strlen(key);
    if (!strncasecmp(c, key, n) && c[n] == '=') snprintf(out, cap, "%s", c + n + 1);
}

static int open_ogg(struct adec *d, struct adec_tags *t, const char **err) {
    int e = 0;
    d->v = stb_vorbis_open_memory(d->file, (int)d->size, &e, NULL);
    if (!d->v) { *err = "can't decode this Ogg file (Vorbis only)"; return -1; }
    stb_vorbis_info vi = stb_vorbis_get_info(d->v);
    d->kind = K_OGG; d->rate = vi.sample_rate; d->chans = vi.channels;
    d->len_ms = (int)((int64_t)stb_vorbis_stream_length_in_samples(d->v) * 1000 / d->rate);
    if (t) {
        stb_vorbis_comment vc = stb_vorbis_get_comment(d->v);
        for (int i = 0; i < vc.comment_list_length; i++) {
            vorbis_tag(t->title, sizeof t->title, vc.comment_list[i], "TITLE");
            vorbis_tag(t->artist, sizeof t->artist, vc.comment_list[i], "ARTIST");
            vorbis_tag(t->album, sizeof t->album, vc.comment_list[i], "ALBUM");
        }
    }
    snprintf(d->info, sizeof d->info, "Ogg Vorbis, %d Hz, %s", d->rate, d->chans == 1 ? "mono" : d->chans == 2 ? "stereo" : "multichannel");
    return 0;
}

struct adec *adec_open(const char *path, struct adec_tags *tags, const char **err) {
    static const char *dummy;
    if (!err) err = &dummy;
    if (tags) memset(tags, 0, sizeof *tags);
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "can't open the file"; return NULL; }
    struct adec *d = calloc(1, sizeof *d);
    if (!d) { fclose(f); *err = "out of memory"; return NULL; }
    uint8_t h[4];
    int r;
    if (fread(h, 1, 4, f) == 4 && !memcmp(h, "RIFF", 4)) {
        rewind(f);
        r = open_wav(d, f, err);
        if (r) fclose(f);
    } else {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        d->file = n > 0 ? malloc(n) : NULL;
        d->size = d->file && fread(d->file, 1, n, f) == (size_t)n ? (size_t)n : 0;
        fclose(f);
        if (!d->size) { *err = n > 0 ? "can't read the file" : "empty file"; r = -1; }
        else r = !memcmp(d->file, "OggS", 4) ? open_ogg(d, tags, err) : open_mp3(d, tags, err);
    }
    if (r) { adec_close(d); return NULL; }
    return d;
}
