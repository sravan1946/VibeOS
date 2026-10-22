/* midi [-l] [-w WAD] FILE.mid : Standard MIDI File player. Synthesises with the OPL3 chip emulator in
 * user/music/ and the GENMIDI instrument lump of a Doom IWAD (default /usr/share/doom/freedoom1.wad). */
#include <audio.h>
#include <vutil.h>
#include "music.h"

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* find the GENMIDI lump in a WAD: 12-byte header (id, lumps, directory offset), 16-byte entries (pos, size, name) */
static int load_genmidi(const char *wad) {
    FILE *f = fopen(wad, "r");
    if (!f) { WARN("midi", wad); return -1; }
    unsigned char h[12], e[16];
    int ok = -1;
    if (fread(h, 1, 12, f) == 12 && (!memcmp(h, "IWAD", 4) || !memcmp(h, "PWAD", 4))) {
        uint32_t n = le32(h + 4);
        fseek(f, le32(h + 8), SEEK_SET);
        for (uint32_t i = 0; i < n && fread(e, 1, 16, f) == 16; i++) {
            if (memcmp(e + 8, "GENMIDI", 8)) continue;
            uint32_t sz = le32(e + 4);
            unsigned char *b = malloc(sz);
            if (b && fseek(f, le32(e), SEEK_SET) == 0 && fread(b, 1, sz, f) == sz) ok = mus_genmidi(b, sz);
            free(b);
            break;
        }
    }
    if (ok < 0) fprintf(stderr, "midi: %s: no GENMIDI instrument lump\n", wad);
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    const char *wad = "/usr/share/doom/freedoom1.wad", *file = NULL;
    int loop = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wad = argv[++i];
        else file = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: midi [-l] [-w WAD] FILE.mid\n"); return 1; }
    int fd0 = open(file, O_RDONLY);
    if (fd0 < 0) { WARN("midi", file); return 1; }
    size_t len;
    char *data = readall(fd0, &len);
    close(fd0);
    if (!data) { WARN("midi", file); return 1; }
    struct song *s = mus_parse_smf((uint8_t *)data, len);
    if (!s) { fprintf(stderr, "midi: %s: not a playable Standard MIDI File (PPQ timing, format 0/1)\n", file); return 1; }
    mus_init();
    if (load_genmidi(wad) < 0) { fprintf(stderr, "midi: instruments come from a Doom WAD; run `make doom-wad` on the host and rebuild\n"); return 1; }
    int fd = audio_open("midi");
    if (fd < 0) return 1;
    uint32_t total = mus_frames(s) / MUS_RATE;
    printf("%s: %u:%02u%s\n", base_name(file), total / 60, total % 60, loop ? " (looping, Ctrl-C to stop)" : "");
    mus_play(s, loop);
    static int acc[1024 * 2];
    static short out[1024 * 2];
    while (mus_playing()) {
        memset(acc, 0, sizeof acc);
        mus_render(acc, 1024);
        for (int i = 0; i < 2048; i++) out[i] = acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i];
        if (write_all(fd, (const char *)out, sizeof out) < 0) { perror("midi"); return 1; }
    }
    close(fd);                                       /* drains */
    return 0;
}
