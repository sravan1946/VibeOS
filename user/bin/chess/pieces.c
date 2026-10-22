/* pieces.c - chess piece sprites drawn from polygons and circles in a 100x100 box, 3x3 supersampled,
 * with an outline and a soft vertical gradient. Rendered once per size by pieces_build(). */
#include "pieces.h"
#include <stdlib.h>
#include <string.h>

struct prim { char k; int n; float v[40]; };   /* 'c' circle {cx,cy,r}, 'p' polygon {x,y ...}, 'e' ellipse {cx,cy,rx,ry} */

#define C(x, y, r)      { 'c', 3, { x, y, r } }
#define E(x, y, a, b)   { 'e', 4, { x, y, a, b } }
#define P(n, ...)       { 'p', n, { __VA_ARGS__ } }

#define BASE  P(6, 24,88, 24,81, 31,76, 69,76, 76,81, 76,88)
static const struct prim PAWN[]   = { C(50,27,11), P(10, 45,36, 55,36, 56,44, 62,49, 60,53, 64,66, 71,78, 29,78, 36,66, 40,53), P(4, 27,78, 73,78, 73,87, 27,87), P(0) };
static const struct prim ROOK[]   = { BASE, P(4, 34,76, 37,40, 63,40, 66,76),
    P(12, 30,41, 30,20, 40,20, 40,27, 46,27, 46,20, 54,20, 54,27, 60,27, 60,20, 70,20, 70,41), P(0) };
static const struct prim BISHOP[] = { BASE, P(4, 41,76, 44,52, 56,52, 59,76), P(4, 37,54, 63,54, 61,48, 39,48),
    E(50,33,13,18), C(50,13,5), P(0) };
static const struct prim QUEEN[]  = { BASE, P(11, 30,28, 35,52, 40,26, 45,48, 50,24, 55,48, 60,26, 65,52, 70,28, 67,74, 33,74),
    C(30,25,4.5f), C(40,22,4.5f), C(50,20,4.5f), C(60,22,4.5f), C(70,25,4.5f), P(0) };
static const struct prim KING[]   = { BASE, P(4, 33,76, 37,42, 63,42, 67,76), E(50,46,19,13),
    P(4, 47,8, 53,8, 53,34, 47,34), P(4, 40,16, 60,16, 60,22, 40,22), P(0) };
static const struct prim KNIGHT[] = { BASE,
    P(17, 31,77, 34,62, 44,49, 35,51, 27,57, 20,53, 22,44, 30,33, 38,25, 40,12, 48,20, 55,14, 59,25, 68,35, 74,53, 76,77, 31,77), P(0) };
/* details painted in the outline colour on top */
static const struct prim D_BISHOP[] = { P(4, 53,22, 56,25, 47,39, 44,36), P(0) };
static const struct prim D_KNIGHT[] = { C(38,35,2.6f), C(23,50,1.6f), P(0) };
static const struct prim D_KING[]   = { P(4, 34,64, 66,64, 66,67, 34,67), P(0) };
static const struct prim D_QUEEN[]  = { P(4, 34,64, 66,64, 66,67, 34,67), P(0) };
static const struct prim D_ROOK[]   = { P(4, 32,60, 68,60, 68,63, 32,63), P(0) };
static const struct prim D_PAWN[]   = { P(0) };

static const struct prim *SHAPE[7] = { 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
static const struct prim *DETAIL[7] = { 0, D_PAWN, D_KNIGHT, D_BISHOP, D_ROOK, D_QUEEN, D_KING };

static int inside(const struct prim *p, float x, float y) {
    if (p->k == 'c') { float dx = x - p->v[0], dy = y - p->v[1]; return dx * dx + dy * dy <= p->v[2] * p->v[2]; }
    if (p->k == 'e') { float dx = (x - p->v[0]) / p->v[2], dy = (y - p->v[1]) / p->v[3]; return dx * dx + dy * dy <= 1; }
    int in = 0, n = p->n;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = p->v[2 * i], yi = p->v[2 * i + 1], xj = p->v[2 * j], yj = p->v[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

static void coverage(const struct prim *list, int S, unsigned char *out) {
    memset(out, 0, S * S);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float u = (x + (sx + 0.5f) / 3) * 100.0f / S, v = (y + (sy + 0.5f) / 3) * 100.0f / S;
                    for (const struct prim *p = list; p->n; p++) if (inside(p, u, v)) { hit++; break; }
                }
            out[y * S + x] = hit * 255 / 9;
        }
}

static uint32_t *sets[2][13];
static int set_size[2];

void pieces_build(int set, int S) {
    if (set_size[set] == S) return;
    for (int i = 0; i < 13; i++) { free(sets[set][i]); sets[set][i] = NULL; }
    set_size[set] = S;
    unsigned char *cov = malloc(S * S), *det = malloc(S * S);
    int r = S >= 48 ? 2 : 1;
    for (int t = 1; t <= 6; t++) {
        coverage(SHAPE[t], S, cov);
        coverage(DETAIL[t], S, det);
        for (int white = 0; white < 2; white++) {
            uint32_t *px = malloc(S * S * 4);
            int fill_top = white ? 0xFFFFFF : 0x5A4B80, fill_bot = white ? 0xCFC3F0 : 0x1E1730;
            int line = white ? 0x2A2140 : 0xE6DDFF;
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++) {
                    int a = cov[y * S + x], oa = a;
                    for (int dy = -r; dy <= r; dy++)
                        for (int dx = -r; dx <= r; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx >= 0 && yy >= 0 && xx < S && yy < S && cov[yy * S + xx] > oa) oa = cov[yy * S + xx];
                        }
                    int t8 = y * 256 / S;      /* vertical gradient */
                    uint32_t fc = mix(0xFF000000u | fill_top, 0xFF000000u | fill_bot, t8);
                    int d = det[y * S + x];
                    if (d > a) d = a;
                    /* outline -> fill -> detail, straight alpha result */
                    int fr = (fc >> 16) & 255, fg = (fc >> 8) & 255, fb = fc & 255;
                    int lr = (line >> 16) & 255, lg = (line >> 8) & 255, lb = line & 255;
                    int cr = (fr * (255 - d) + lr * d) / 255, cg = (fg * (255 - d) + lg * d) / 255, cb = (fb * (255 - d) + lb * d) / 255;
                    int A = a + oa * (255 - a) / 255;
                    if (!A) { px[y * S + x] = 0; continue; }
                    int R = (cr * a + lr * (oa * (255 - a) / 255)) / A, G = (cg * a + lg * (oa * (255 - a) / 255)) / A, Bb = (cb * a + lb * (oa * (255 - a) / 255)) / A;
                    px[y * S + x] = ((uint32_t)A << 24) | (R << 16) | (G << 8) | Bb;
                }
            sets[set][white ? t : 6 + t] = px;
        }
    }
    free(cov); free(det);
}

const uint32_t *piece_sprite(int set, int piece) {
    return sets[set][piece > 0 ? piece : 6 - piece];
}
