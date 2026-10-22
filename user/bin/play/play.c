/* play [-v 0-100] FILE... : WAV, MP3 or Ogg Vorbis -> /dev/dsp (decoded and resampled to 48 kHz by user/music/adec.c).
 * Mixes with whatever else is playing; each file is its own /dev/dsp stream, -v sets this stream's volume. */
#include <audio.h>
#include <stdlib.h>
#include <vutil.h>
#include "adec.h"

#define CHUNK 2048                              /* frames per write() */

int main(int argc, char **argv) {
    int a = 1, svol = 100;
    if (argc > 2 && !strcmp(argv[1], "-v")) { svol = atoi(argv[2]); a = 3; }
    if (argc <= a) { fprintf(stderr, "usage: play [-v 0-100] FILE.wav|.mp3|.ogg...\n"); return 1; }
    static int16_t out[CHUNK * 2];
    int rc = 0;
    for (int i = a; i < argc; i++) {
        const char *err;
        struct adec_tags t;
        struct adec *d = adec_open(argv[i], &t, &err);
        if (!d) { fprintf(stderr, "play: %s: %s\n", argv[i], err); rc = 1; continue; }
        int fd = audio_open("play");
        if (fd < 0) return 1;
        ioctl(fd, AUDIO_SETSVOL, &svol);
        int secs = adec_length_ms(d) / 1000;
        printf("%s: %s, %d:%02d\n", base_name(argv[i]), adec_info(d), secs / 60, secs % 60);
        if (t.title[0]) printf("  %s%s%s\n", t.title, t.artist[0] ? " - " : "", t.artist);
        int n;
        while ((n = adec_read(d, out, CHUNK)) > 0)
            if (write_all(fd, (const char *)out, n * 4) < 0) { perror("play"); rc = 1; break; }
        close(fd);                              /* drains */
        adec_close(d);
    }
    return rc;
}
