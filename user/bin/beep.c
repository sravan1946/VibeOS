/* beep [freq_hz [ms]] : PC speaker, default 440 Hz for 200 ms */
#include <vutil.h>
#include <syscall.h>

int main(int argc, char **argv) {
    unsigned hz = argc > 1 ? atoi(argv[1]) : 440, ms = argc > 2 ? atoi(argv[2]) : 200;
    if (beep(hz, ms) < 0) { fprintf(stderr, "beep: %s (freq 20-20000 Hz, at most 5000 ms)\n", strerror(errno)); return 1; }
    return 0;
}
