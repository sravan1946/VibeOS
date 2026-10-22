/* image.h - PNG/BMP/PPM/JPEG/GIF decoding, PNG encoding, resampling. Pixels are 0xAARRGGBB, not premultiplied
 * (the gfx format). Also builds on the host (tools/imagetest). Link libgui.a before libc.a. */
#pragma once
#include <stdint.h>
#include <stddef.h>

struct image { int w, h; uint32_t *px; };      /* px is malloc'd, w*h entries */

/* An animation: n full-canvas frames (each w*h pixels, malloc'd, already composited), delay[i] in ms. loop is
 * the play count (0 = forever). Still pictures are 1 frame. GIF delays are as stored (0 happens; players clamp). */
struct image_anim { int w, h, n, loop; uint32_t **px; int *delay; };

/* 0 on success, -1 on a bad/unsupported file. Format is detected from the magic bytes. */
int  image_decode(const uint8_t *data, size_t n, struct image *im);
int  image_load(const char *path, struct image *im);
void image_free(struct image *im);
int  image_is_ext(const char *name);           /* 1 if the file name ends in .png/.bmp/.ppm/.pgm/.jpg/.jpeg/.gif */

/* All frames of a GIF (other formats give one frame). 0 / -1. image_decode on a GIF returns the first frame only. */
int  image_decode_anim(const uint8_t *data, size_t n, struct image_anim *a);
int  image_load_anim(const char *path, struct image_anim *a);
void image_anim_free(struct image_anim *a);

/* the codecs behind image_decode: JPEG (baseline + progressive, EXIF orientation applied) and GIF (maxf frames, 0 = all) */
int  jpeg_decode(const uint8_t *d, size_t n, struct image *im);
int  gif_decode(const uint8_t *d, size_t n, int maxf, struct image_anim *a);

/* malloc'd PNG (RGB if fully opaque, else RGBA) with LZ77 + fixed Huffman; caller frees. NULL on OOM. */
uint8_t *image_png_encode(const uint32_t *px, int w, int h, size_t *len);
int  image_save_png(const char *path, const uint32_t *px, int w, int h);   /* 0 / -1; retries transient EIO */

/* Render the source into dst (dw x dh, stride in pixels): dst(x,y) shows source point
 * (ox + (x+.5)/scale, oy + (y+.5)/scale). Pixels outside the image get alpha 0. scale < 1 area-averages,
 * otherwise bilinear (smooth) or nearest. Colours are alpha-weighted so transparency doesn't fringe. */
void image_sample(const struct image *im, uint32_t *dst, int dw, int dh, int stride,
                  float ox, float oy, float scale, int smooth);

/* zlib (RFC 1950/1951) building blocks, exposed for tests and other formats */
int  zlib_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *olen, size_t hint);   /* 0 / -1; malloc'd */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n);   /* start with 0 */
uint32_t adler32(const uint8_t *p, size_t n);
