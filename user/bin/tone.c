/* tone [hz [ms]] : sine test tone on /dev/dsp (default 440 Hz, 1000 ms) */
#include <audio.h>
#include <stdlib.h>
#include <math.h>
#include <vutil.h>

#define N 1024
int main(int argc, char **argv) {
    int hz = argc > 1 ? atoi(argv[1]) : 440, ms = argc > 2 ? atoi(argv[2]) : 1000;
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 600000) { fprintf(stderr, "usage: tone [hz 20-20000 [ms]]\n"); return 1; }
    int fd = audio_open("tone");
    if (fd < 0) return 1;
    long total = (long)ms * AUDIO_RATE / 1000, fade = AUDIO_RATE / 200;   /* 5 ms ramps avoid clicks */
    double ph = 0, step = 2 * M_PI * hz / AUDIO_RATE;
    static short b[N * 2];
    for (long i = 0; i < total;) {
        int n = total - i < N ? (int)(total - i) : N;
        for (int j = 0; j < n; j++, i++) {
            double g = 0.5;
            if (i < fade) g *= (double)i / fade;
            if (total - i < fade) g *= (double)(total - i) / fade;
            b[2 * j] = b[2 * j + 1] = (short)(sin(ph) * g * 32767);
            ph += step;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
        }
        if (write_all(fd, (const char *)b, n * 4) < 0) { perror("tone"); return 1; }
    }
    close(fd);                                  /* drains */
    return 0;
}
