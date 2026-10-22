/* play FILE.wav : PCM WAV (8/16-bit, mono/stereo, any rate) -> /dev/dsp, linearly resampled to 48 kHz */
#include <audio.h>
#include <stdlib.h>
#include <vutil.h>

static FILE *f;
static int bits, chans, left;                 /* left = data bytes not yet read */
static unsigned char ib[16384];
static int ipos, ilen;

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* next source frame as 16-bit L/R; 0 at the end */
static int frame(int *l, int *r) {
    int fb = chans * bits / 8;
    if (ipos + fb > ilen) {
        int want = left < (int)sizeof ib ? left : (int)sizeof ib;
        want -= want % fb;
        ilen = want ? (int)fread(ib, 1, want, f) : 0;
        left -= ilen; ipos = 0;
        if (ilen < fb) return 0;
    }
    unsigned char *p = ib + ipos;
    ipos += fb;
    if (bits == 8) { *l = (p[0] - 128) << 8; *r = chans == 2 ? (p[1] - 128) << 8 : *l; }
    else { *l = (short)(p[0] | p[1] << 8); *r = chans == 2 ? (short)(p[2] | p[3] << 8) : *l; }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: play FILE.wav\n"); return 1; }
    f = fopen(argv[1], "r");
    if (!f) { WARN("play", argv[1]); return 1; }
    unsigned char h[12], ch[8];
    int rate = 0, tag = 0, ok = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fprintf(stderr, "play: %s: not a WAV file\n", argv[1]); return 1; }
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t n = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4) && n >= 16) {
            unsigned char fm[40];
            uint32_t k = n < sizeof fm ? n : sizeof fm;
            if (fread(fm, 1, k, f) != k) break;
            tag = fm[0] | fm[1] << 8; chans = fm[2] | fm[3] << 8; rate = le32(fm + 4); bits = fm[14] | fm[15] << 8;
            if (tag == 0xFFFE && k >= 26) tag = fm[24] | fm[25] << 8;      /* WAVE_FORMAT_EXTENSIBLE sub-format */
            fseek(f, n - k + (n & 1), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) { left = n; ok = 1; break; }
        else fseek(f, n + (n & 1), SEEK_CUR);
    }
    if (!ok || tag != 1 || (bits != 8 && bits != 16) || chans < 1 || chans > 2 || rate < 1000 || rate > 192000) {
        fprintf(stderr, "play: %s: unsupported (need PCM 8/16-bit, 1-2 channels)\n", argv[1]);
        return 1;
    }
    int fd = audio_open("play");
    if (fd < 0) return 1;
    int secs = left / (chans * bits / 8) / rate;
    printf("%s: %d Hz, %d-bit, %s, %d:%02d\n", base_name(argv[1]), rate, bits, chans == 2 ? "stereo" : "mono", secs / 60, secs % 60);
    uint32_t step = (uint32_t)((uint64_t)rate * 65536 / AUDIO_RATE), frac = 0;    /* source frames per output frame, 16.16 */
    int al, ar, bl, br;
    static short out[2048 * 2];
    int n = 0;
    if (!frame(&al, &ar) || !frame(&bl, &br)) { fprintf(stderr, "play: %s: no audio data\n", argv[1]); return 1; }
    for (;;) {
        out[2 * n] = al + (int)(((int64_t)(bl - al) * (int)frac) >> 16);
        out[2 * n + 1] = ar + (int)(((int64_t)(br - ar) * (int)frac) >> 16);
        if (++n == 2048) { if (write_all(fd, (const char *)out, n * 4) < 0) { perror("play"); return 1; } n = 0; }
        frac += step;
        for (; frac >= 65536; frac -= 65536) {
            al = bl; ar = br;
            if (!frame(&bl, &br)) goto done;
        }
    }
done:
    if (n) write_all(fd, (const char *)out, n * 4);
    close(fd);                                  /* drains */
    return 0;
}
