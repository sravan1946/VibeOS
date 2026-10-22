/* music.h - OPL3 MIDI synth + sequencer shared by Doom's music module and /bin/midi.
 * A song is a time-sorted list of MIDI events (times in 48 kHz frames); mus_render() runs the
 * sequencer and the Nuked-OPL3 chip together, so tempo follows however fast the caller pulls audio. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MUS_RATE 48000

struct song;

int mus_init(void);                                  /* reset the chip; 0 or -1 */
int mus_genmidi(const uint8_t *lump, size_t len);    /* instrument bank (Doom's GENMIDI lump); copied; 0 or -1 */
struct song *mus_parse_mus(const uint8_t *d, size_t n);   /* Doom MUS lump */
struct song *mus_parse_smf(const uint8_t *d, size_t n);   /* Standard MIDI File, format 0/1, PPQ timing */
void mus_free(struct song *s);
uint32_t mus_frames(const struct song *s);           /* length in frames */

void mus_play(struct song *s, int loop);             /* starts from the top */
void mus_stop(void);
void mus_pause(int on);
int mus_playing(void);                               /* 1 until the song (and its release tail) has ended */
void mus_volume(int v);                              /* 0..127 */
uint32_t mus_pos(void);                              /* frames played so far */
void mus_render(int *acc, int frames);               /* ADD interleaved stereo s16-scale samples into acc */
