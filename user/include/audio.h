/* audio.h - /dev/dsp: write AUDIO_RATE (48 kHz) stereo s16le frames; write() blocks while the ring is full */
#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

/* fd or -1 after printing why */
static inline int audio_open(const char *prog) {
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) fprintf(stderr, "%s: no audio device (/dev/dsp): %s\n", prog, strerror(errno));
    return fd;
}
/* bytes queued in the DMA ring */
static inline int audio_queued(int fd) { int q = 0; ioctl(fd, AUDIO_GETQ, &q); return q < 0 ? 0 : q; }
