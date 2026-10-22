/* audio.h - /dev/dsp: write AUDIO_RATE (48 kHz) stereo s16le frames; write() blocks while this open's queue (64 KiB) is full.
 * Every open is its own stream and the kernel mixes them, so several programs can play at once. */
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
/* bytes between write() and the speaker (this stream's queue + the DMA ring) */
static inline int audio_queued(int fd) { int q = 0; ioctl(fd, AUDIO_GETQ, &q); return q < 0 ? 0 : q; }
