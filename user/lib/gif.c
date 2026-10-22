/* gif.c - GIF87a/89a decoder for libgui (see image.h): LZW, global/local palettes, interlace, transparency, the
 * three disposal modes. Every frame comes out as the full composited canvas (RGBA), so a player just shows frame i.
 * Disposal to background clears to transparent (what browsers do), not to the palette's background colour.
 * Frames beyond GIF_MAXMEM of canvases are dropped (the animation is cut there). Truncated files keep the frames
 * (and the part of the last frame) that decoded. */
#include "image.h"
#include <stdlib.h>
#include <string.h>

#define GIF_MAXMEM (48u << 20)

struct lz { const uint8_t *d; size_t n, pos; int left, end; };

static int lz_byte(struct lz *z) {              /* next byte of the sub-block chain, -1 at the terminator / EOF */
    while (!z->left) {
        if (z->end || z->pos >= z->n) { z->end = 1; return -1; }
        z->left = z->d[z->pos++];
        if (!z->left) { z->end = 1; return -1; }
    }
    if (z->pos >= z->n) { z->end = 1; return -1; }
    z->left--;
    return z->d[z->pos++];
}

static int irow(int y, int h) {                 /* interlaced: y-th stored row -> picture row */
    int p0 = (h + 7) / 8, p1 = (h + 3) / 8, p2 = (h + 1) / 4;
    if (y < p0) return y * 8;
    y -= p0;
    if (y < p1) return 4 + y * 8;
    y -= p1;
    if (y < p2) return 2 + y * 4;
    return 1 + (y - p2) * 2;
}

static int le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* decode one frame's LZW data into the canvas at (fx, fy), w x h. Returns pixels produced. */
static long lzw_draw(struct lz *z, int minbits, uint32_t *cv, int W, int H, int fx, int fy, int w, int h,
                     int ilace, const uint32_t *pal, int tr) {
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4100];
    int clear = 1 << minbits, eoi = clear + 1, next = clear + 2, size = minbits + 1, prev = -1, first = 0;
    uint32_t bits = 0;
    int nb = 0, x = 0, y = 0;
    long cnt = 0, total = (long)w * h;
    uint32_t *row = NULL;
    int rowy = -1;
    while (cnt < total) {
        while (nb < size) {
            int b = lz_byte(z);
            if (b < 0) return cnt;
            bits |= (uint32_t)b << nb;
            nb += 8;
        }
        int code = bits & ((1 << size) - 1);
        bits >>= size; nb -= size;
        if (code == clear) { size = minbits + 1; next = clear + 2; prev = -1; continue; }
        if (code == eoi) break;
        int sp = 0, cur = code;
        if (prev < 0) {
            if (code >= clear) break;
            stack[sp++] = code; first = code;
        } else {
            if (code >= next) {
                if (code > next) break;                 /* corrupt */
                stack[sp++] = first;
                cur = prev;
            }
            while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
            first = cur;
            stack[sp++] = cur;
            if (next < 4096) {
                prefix[next] = prev; suffix[next] = first; next++;
                if (next == 1 << size && size < 12) size++;
            }
        }
        prev = code;
        while (sp && cnt < total) {
            int v = stack[--sp];
            if (y != rowy) {
                int r = fy + (ilace ? irow(y, h) : y);
                row = r < H ? cv + (size_t)r * W + fx : NULL;
                rowy = y;
            }
            if (row && fx + x < W && v != tr) row[x] = pal[v];
            cnt++;
            if (++x == w) { x = 0; y++; }
        }
    }
    return cnt;
}

int gif_decode(const uint8_t *d, size_t n, int maxf, struct image_anim *a) {
    memset(a, 0, sizeof *a);
    if (n < 13 || memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9') || d[5] != 'a') return -1;
    int W = le16(d + 6), H = le16(d + 8), fl = d[10];
    size_t pos = 13, cbytes = (size_t)W * H * 4;
    uint32_t gpal[256], pal[256];
    for (int i = 0; i < 256; i++) gpal[i] = 0xFF000000u | i * 0x010101u;      /* no table: greyscale ramp */
    if (!W || !H || (uint64_t)W * H > 1u << 25) return -1;
    if (fl & 0x80) {
        int nc = 2 << (fl & 7);
        if (pos + 3 * nc > n) return -1;
        for (int i = 0; i < nc; i++, pos += 3) gpal[i] = 0xFF000000u | d[pos] << 16 | d[pos + 1] << 8 | d[pos + 2];
        for (int i = nc; i < 256; i++) gpal[i] = 0xFF000000u;
    }
    uint32_t *cv = calloc(1, cbytes), *saved = NULL;
    if (!cv) return -1;
    int maxn = (int)(GIF_MAXMEM / cbytes);
    if (maxn < 1) maxn = 1;
    if (maxf > 0 && maxf < maxn) maxn = maxf;
    int cap = 0, disp = 0, tr = -1, delay = 0, pdisp = 0, px_ = 0, py_ = 0, pw_ = 0, ph_ = 0, loop = 1;
    while (pos < n && a->n < maxn) {
        int b = d[pos++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (pos >= n) break;
            int label = d[pos++];
            if (label == 0xF9 && pos + 5 <= n && d[pos] == 4) {
                disp = (d[pos + 1] >> 2) & 7;
                tr = d[pos + 1] & 1 ? d[pos + 4] : -1;
                delay = le16(d + pos + 2) * 10;
            } else if (label == 0xFF && pos + 16 <= n && d[pos] == 11 && !memcmp(d + pos + 1, "NETSCAPE2.0", 11) &&
                       d[pos + 12] == 3 && d[pos + 13] == 1)
                loop = le16(d + pos + 14);
            while (pos < n && d[pos]) pos += d[pos] + 1;
            pos++;
            continue;
        }
        if (b != 0x2C || pos + 9 > n) break;
        int fx = le16(d + pos), fy = le16(d + pos + 2), w = le16(d + pos + 4), h = le16(d + pos + 6), pk = d[pos + 8];
        pos += 9;
        memcpy(pal, gpal, sizeof pal);
        if (pk & 0x80) {
            int nc = 2 << (pk & 7);
            if (pos + 3 * nc > n) break;
            for (int i = 0; i < nc; i++, pos += 3) pal[i] = 0xFF000000u | d[pos] << 16 | d[pos + 1] << 8 | d[pos + 2];
            for (int i = nc; i < 256; i++) pal[i] = 0xFF000000u;
        }
        if (pos >= n || !w || !h) break;
        int minbits = d[pos++];
        if (minbits < 2 || minbits > 8) break;
        if (pdisp == 2) {                               /* undo the previous frame */
            for (int y = py_; y < py_ + ph_ && y < H; y++)
                for (int x = px_; x < px_ + pw_ && x < W; x++) cv[(size_t)y * W + x] = 0;
        } else if (pdisp == 3 && saved) memcpy(cv, saved, cbytes);
        if (disp == 3) {
            if (!saved && !(saved = malloc(cbytes))) disp = 1;
            else memcpy(saved, cv, cbytes);
        }
        struct lz z = { d, n, pos, 0, 0 };
        lzw_draw(&z, minbits, cv, W, H, fx, fy, w, h, pk & 0x40, pal, tr);
        while (!z.end) lz_byte(&z), z.pos += z.left, z.left = 0;     /* skip the rest of the data */
        pos = z.pos;
        if (a->n == cap) {
            int nc = cap ? cap * 2 : 8;
            uint32_t **np = realloc(a->px, nc * sizeof *np);
            int *nd = realloc(a->delay, nc * sizeof *nd);
            if (np) a->px = np;
            if (nd) a->delay = nd;
            if (!np || !nd) break;
            cap = nc;
        }
        uint32_t *fr = malloc(cbytes);
        if (!fr) break;
        memcpy(fr, cv, cbytes);
        a->px[a->n] = fr; a->delay[a->n++] = delay;
        pdisp = disp; px_ = fx; py_ = fy; pw_ = w; ph_ = h;
        disp = 0; tr = -1; delay = 0;
    }
    free(cv); free(saved);
    a->w = W; a->h = H; a->loop = loop;
    if (!a->n) { image_anim_free(a); return -1; }
    return 0;
}
