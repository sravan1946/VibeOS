/* adec.h - audio file decoder for /bin/play and /bin/music: WAV (PCM 8/16-bit), MP3 (minimp3) and Ogg Vorbis
 * (stb_vorbis) to 48 kHz stereo s16le, the format /dev/dsp takes. Also builds on the host (tools/adectest). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ADEC_RATE 48000

struct adec_tags {
    char title[96], artist[96], album[96];      /* UTF-8, empty when the file has none */
    const uint8_t *art;                         /* embedded picture (ID3 APIC: JPEG or PNG), valid until adec_close */
    size_t art_len;
};

struct adec;
/* NULL if the file can't be opened or decoded; *err then says why. tags may be NULL. */
struct adec *adec_open(const char *path, struct adec_tags *tags, const char **err);
/* up to `frames` stereo frames at ADEC_RATE into out; 0 at the end of the stream */
int  adec_read(struct adec *d, int16_t *out, int frames);
int  adec_seek(struct adec *d, int ms);         /* 0, or -1 if the stream can't seek */
int  adec_length_ms(struct adec *d);
const char *adec_info(struct adec *d);          /* e.g. "MP3, 44100 Hz, stereo, 128 kbps" */
void adec_close(struct adec *d);
int  adec_is_audio(const char *name);           /* 1 if the name ends in .wav/.mp3/.ogg */
