/* imagetest - host driver for user/lib/image.c, run by test.py (which makes the files with Pillow).
 *   imagetest dec FILE REF   decode FILE, compare with REF (u32 w, u32 h, RGBA bytes); exit 0 if identical
 *   imagetest dec FILE REF TOL  same, but each colour channel may differ by up to TOL (JPEG)
 *   imagetest anim FILE REF  decode every frame of a GIF; REF is u32 w, h, n, then per frame u32 delay_ms + RGBA; exact
 *   imagetest enc REF OUT    PNG-encode REF (same layout) to OUT
 *   imagetest fuzz FILE      decode many corrupted copies of FILE; the sanitizers catch bad accesses */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) exit(2);
    fclose(f);
    return b;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);                                   /* messages must survive a LeakSanitizer exit */
    if (argc == 4 && !strcmp(argv[1], "anim")) {
        size_t n, rn;
        uint8_t *d = slurp(argv[2], &n), *ref = slurp(argv[3], &rn);
        struct image_anim a;
        if (image_decode_anim(d, n, &a)) { printf("decode failed\n"); return 1; }
        uint32_t w = rd32(ref), h = rd32(ref + 4), nf = rd32(ref + 8);
        if ((uint32_t)a.w != w || (uint32_t)a.h != h || (uint32_t)a.n != nf) { printf("got %dx%d x%d, want %ux%u x%u\n", a.w, a.h, a.n, w, h, nf); return 1; }
        const uint8_t *r = ref + 12;
        for (uint32_t f = 0; f < nf; f++, r += 4 + (size_t)w * h * 4) {
            if ((uint32_t)a.delay[f] != rd32(r)) { printf("frame %u delay %d want %u\n", f, a.delay[f], rd32(r)); return 1; }
            for (size_t i = 0; i < (size_t)w * h; i++) {
                uint32_t p = a.px[f][i], want = (uint32_t)r[4 + 4 * i + 3] << 24 | r[4 + 4 * i] << 16 | r[4 + 4 * i + 1] << 8 | r[4 + 4 * i + 2];
                if ((p >> 24) == 0 && (want >> 24) == 0) continue;
                if (p != want) { printf("frame %u pixel %zu (%zu,%zu): got %08x want %08x\n", f, i, i % w, i / w, p, want); return 1; }
            }
        }
        image_anim_free(&a);
        free(d); free(ref);
        return 0;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "dec")) {
        int tol = argc == 5 ? atoi(argv[4]) : 0, worst = 0;
        size_t n, rn;
        uint8_t *d = slurp(argv[2], &n), *ref = slurp(argv[3], &rn);
        struct image im;
        if (image_decode(d, n, &im)) { printf("decode failed\n"); return 1; }
        uint32_t w = ref[0] | ref[1] << 8 | ref[2] << 16 | (uint32_t)ref[3] << 24, h = ref[4] | ref[5] << 8 | ref[6] << 16 | (uint32_t)ref[7] << 24;
        if ((uint32_t)im.w != w || (uint32_t)im.h != h) { printf("size %dx%d, want %ux%u\n", im.w, im.h, w, h); return 1; }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint32_t p = im.px[i], want = (uint32_t)ref[8 + 4 * i + 3] << 24 | ref[8 + 4 * i] << 16 | ref[8 + 4 * i + 1] << 8 | ref[8 + 4 * i + 2];
            if ((p >> 24) == 0 && (want >> 24) == 0) continue;           /* colour under alpha 0 is don't-care */
            for (int sh = 0; sh < 32; sh += 8) { int dd = abs((int)(p >> sh & 255) - (int)(want >> sh & 255)); if (dd > worst) worst = dd; }
            if (worst > tol) { printf("pixel %zu (%zu,%zu): got %08x want %08x\n", i, i % w, i / w, p, want); return 1; }
        }
        if (tol) printf("max diff %d\n", worst);
        image_free(&im);
        free(d); free(ref);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "enc")) {
        size_t rn, on;
        uint8_t *ref = slurp(argv[2], &rn);
        uint32_t w = ref[0] | ref[1] << 8 | ref[2] << 16 | (uint32_t)ref[3] << 24, h = ref[4] | ref[5] << 8 | ref[6] << 16 | (uint32_t)ref[7] << 24;
        uint32_t *px = malloc((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; i++) px[i] = (uint32_t)ref[8 + 4 * i + 3] << 24 | ref[8 + 4 * i] << 16 | ref[8 + 4 * i + 1] << 8 | ref[8 + 4 * i + 2];
        uint8_t *o = image_png_encode(px, w, h, &on);
        if (!o) return 1;
        FILE *f = fopen(argv[3], "wb");
        fwrite(o, 1, on, f);
        fclose(f);
        free(o); free(px); free(ref);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "fuzz")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n), *m = malloc(n);
        srand(1);
        for (int it = 0; it < 300; it++) {
            memcpy(m, d, n);
            size_t len = it % 5 == 0 ? (size_t)rand() % n : n;           /* truncate sometimes */
            for (int k = 0, nk = 1 + rand() % 8; k < nk; k++) m[rand() % n] = rand();
            struct image im;
            if (!image_decode(m, len, &im)) image_free(&im);
            struct image_anim a;
            if (!image_decode_anim(m, len, &a)) image_anim_free(&a);
        }
        free(d); free(m);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "sample")) {                      /* resampler sanity checks */
        uint32_t src[16], out[64];
        for (int i = 0; i < 16; i++) src[i] = 0xFF000000u | (i % 4 < 2 ? 0x204060 : 0xA0C0E0);       /* 4x4: two flat halves */
        struct image im = { 4, 4, src };
        image_sample(&im, out, 4, 4, 4, 0, 0, 1, 1);                    /* 1:1 is a copy */
        if (memcmp(out, src, sizeof src)) { puts("1:1 differs"); return 1; }
        image_sample(&im, out, 2, 2, 2, 0, 0, 0.5f, 1);                 /* 2x2 box average of flat blocks */
        if (out[0] != 0xFF204060u || out[1] != 0xFFA0C0E0u) { printf("box: %08x %08x\n", out[0], out[1]); return 1; }
        image_sample(&im, out, 8, 8, 8, 0, 0, 2, 1);                    /* bilinear keeps flat areas flat, blends the seam */
        if (out[0] != 0xFF204060u || out[7] != 0xFFA0C0E0u || (out[3] & 255) <= 0x60 || (out[3] & 255) >= 0xE0) { puts("bilinear"); return 1; }
        image_sample(&im, out, 8, 8, 8, -2, 0, 2, 1);                   /* outside the image is transparent */
        if (out[0] >> 24 || out[7 * 8 + 7] >> 24 != 255) { puts("outside"); return 1; }
        uint32_t tr[4] = { 0xFFFFFFFFu, 0x00FF0000u, 0xFFFFFFFFu, 0x00FF0000u };     /* white | transparent red */
        struct image ti = { 2, 2, tr };
        image_sample(&ti, out, 8, 8, 8, 0, 0, 4, 1);
        for (int i = 0; i < 64; i++) if ((out[i] >> 24) && (out[i] & 0xFFFFFF) != 0xFFFFFF) { printf("fringe %08x\n", out[i]); return 1; }
        return 0;
    }
    fprintf(stderr, "usage: see source\n");
    return 2;
}
