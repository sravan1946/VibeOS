/* webp.c - WebP decoder for libgui (see image.h): lossy (VP8 key frames, RFC 6386: intra prediction, DCT, both loop filters,
 * libwebp's "fancy" chroma upsampling so the output matches it), lossless (VP8L: prefix codes, LZ77, colour cache and the four
 * transforms), alpha (ALPH, raw or VP8L-compressed, with the prediction filters) and the extended container (VP8X). Of an
 * animated file the first frame is decoded. Written from the two specifications; the probability/quantiser tables are the
 * specifications' constants. Checked against Pillow's libwebp by tools/imagetest (bit-exact for lossless, within rounding for lossy).
 * Every read is bounds-checked (truncated or corrupt data fails or yields zero bits), so hostile files cannot crash it. */
#include "image.h"
#include <stdlib.h>
#include <string.h>

#define WEBP_MAXPIX (80u << 20)

static const uint8_t kcoef[1056] = {
    128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,
    253,136,254,255,228,219,128,128,128,128,128,189,129,242,255,227,213,255,219,128,128,128,106,126,227,252,214,209,255,255,128,128,128,
    1,98,248,255,236,226,255,255,128,128,128,181,133,238,254,221,234,255,154,128,128,128,78,134,202,247,198,180,255,219,128,128,128,
    1,185,249,255,243,255,128,128,128,128,128,184,150,247,255,236,224,128,128,128,128,128,77,110,216,255,236,230,128,128,128,128,128,
    1,101,251,255,241,255,128,128,128,128,128,170,139,241,252,236,209,255,255,128,128,128,37,116,196,243,228,255,255,255,128,128,128,
    1,204,254,255,245,255,128,128,128,128,128,207,160,250,255,238,128,128,128,128,128,128,102,103,231,255,211,171,128,128,128,128,128,
    1,152,252,255,240,255,128,128,128,128,128,177,135,243,255,234,225,128,128,128,128,128,80,129,211,255,194,224,128,128,128,128,128,
    1,1,255,128,128,128,128,128,128,128,128,246,1,255,128,128,128,128,128,128,128,128,255,128,128,128,128,128,128,128,128,128,128,
    198,35,237,223,193,187,162,160,145,155,62,131,45,198,221,172,176,220,157,252,221,1,68,47,146,208,149,167,221,162,255,223,128,
    1,149,241,255,221,224,255,255,128,128,128,184,141,234,253,222,220,255,199,128,128,128,81,99,181,242,176,190,249,202,255,255,128,
    1,129,232,253,214,197,242,196,255,255,128,99,121,210,250,201,198,255,202,128,128,128,23,91,163,242,170,187,247,210,255,255,128,
    1,200,246,255,234,255,128,128,128,128,128,109,178,241,255,231,245,255,255,128,128,128,44,130,201,253,205,192,255,255,128,128,128,
    1,132,239,251,219,209,255,165,128,128,128,94,136,225,251,218,190,255,255,128,128,128,22,100,174,245,186,161,255,199,128,128,128,
    1,182,249,255,232,235,128,128,128,128,128,124,143,241,255,227,234,128,128,128,128,128,35,77,181,251,193,211,255,205,128,128,128,
    1,157,247,255,236,231,255,255,128,128,128,121,141,235,255,225,227,255,255,128,128,128,45,99,188,251,195,217,255,224,128,128,128,
    1,1,251,255,213,255,128,128,128,128,128,203,1,248,255,255,128,128,128,128,128,128,137,1,177,255,224,255,128,128,128,128,128,
    253,9,248,251,207,208,255,192,128,128,128,175,13,224,243,193,185,249,198,255,255,128,73,17,171,221,161,179,236,167,255,234,128,
    1,95,247,253,212,183,255,255,128,128,128,239,90,244,250,211,209,255,255,128,128,128,155,77,195,248,188,195,255,255,128,128,128,
    1,24,239,251,218,219,255,205,128,128,128,201,51,219,255,196,186,128,128,128,128,128,69,46,190,239,201,218,255,228,128,128,128,
    1,191,251,255,255,128,128,128,128,128,128,223,165,249,255,213,255,128,128,128,128,128,141,124,248,255,255,128,128,128,128,128,128,
    1,16,248,255,255,128,128,128,128,128,128,190,36,230,255,236,255,128,128,128,128,128,149,1,255,128,128,128,128,128,128,128,128,
    1,226,255,128,128,128,128,128,128,128,128,247,192,255,128,128,128,128,128,128,128,128,240,128,255,128,128,128,128,128,128,128,128,
    1,134,252,255,255,128,128,128,128,128,128,213,62,250,255,255,128,128,128,128,128,128,55,93,255,128,128,128,128,128,128,128,128,
    128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,128,
    202,24,213,235,186,191,220,160,240,175,255,126,38,182,232,169,184,228,174,255,187,128,61,46,138,219,151,178,240,170,255,216,128,
    1,112,230,250,199,191,247,159,255,255,128,166,109,228,252,211,215,255,174,128,128,128,39,77,162,232,172,180,245,178,255,255,128,
    1,52,220,246,198,199,249,220,255,255,128,124,74,191,243,183,193,250,221,255,255,128,24,71,130,219,154,170,243,182,255,255,128,
    1,182,225,249,219,240,255,224,128,128,128,149,150,226,252,216,205,255,171,128,128,128,28,108,170,242,183,194,254,223,255,255,128,
    1,81,230,252,204,203,255,192,128,128,128,123,102,209,247,188,196,255,233,128,128,128,20,95,153,243,164,173,255,203,128,128,128,
    1,222,248,255,216,213,128,128,128,128,128,168,175,246,252,235,205,255,255,128,128,128,47,116,215,255,211,212,255,255,128,128,128,
    1,121,236,253,212,214,255,255,128,128,128,141,84,213,252,201,202,255,219,128,128,128,42,80,160,240,162,185,255,205,128,128,128,
    1,1,255,128,128,128,128,128,128,128,128,244,1,255,128,128,128,128,128,128,128,128,238,1,255,128,128,128,128,128,128,128,128,
};
static const uint8_t kupd[1056] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    176,246,255,255,255,255,255,255,255,255,255,223,241,252,255,255,255,255,255,255,255,255,249,253,253,255,255,255,255,255,255,255,255,
    255,244,252,255,255,255,255,255,255,255,255,234,254,254,255,255,255,255,255,255,255,255,253,255,255,255,255,255,255,255,255,255,255,
    255,246,254,255,255,255,255,255,255,255,255,239,253,254,255,255,255,255,255,255,255,255,254,255,254,255,255,255,255,255,255,255,255,
    255,248,254,255,255,255,255,255,255,255,255,251,255,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,253,254,255,255,255,255,255,255,255,255,251,254,254,255,255,255,255,255,255,255,255,254,255,254,255,255,255,255,255,255,255,255,
    255,254,253,255,254,255,255,255,255,255,255,250,255,254,255,254,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    217,255,255,255,255,255,255,255,255,255,255,225,252,241,253,255,255,254,255,255,255,255,234,250,241,250,253,255,253,254,255,255,255,
    255,254,255,255,255,255,255,255,255,255,255,223,254,254,255,255,255,255,255,255,255,255,238,253,254,254,255,255,255,255,255,255,255,
    255,248,254,255,255,255,255,255,255,255,255,249,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,253,255,255,255,255,255,255,255,255,255,247,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,253,254,255,255,255,255,255,255,255,255,252,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,254,254,255,255,255,255,255,255,255,255,253,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,254,253,255,255,255,255,255,255,255,255,250,255,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    186,251,250,255,255,255,255,255,255,255,255,234,251,244,254,255,255,255,255,255,255,255,251,251,243,253,254,255,254,255,255,255,255,
    255,253,254,255,255,255,255,255,255,255,255,236,253,254,255,255,255,255,255,255,255,255,251,253,253,254,254,255,255,255,255,255,255,
    255,254,254,255,255,255,255,255,255,255,255,254,254,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,254,255,255,255,255,255,255,255,255,255,254,254,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    248,255,255,255,255,255,255,255,255,255,255,250,254,252,254,255,255,255,255,255,255,255,248,254,249,253,255,255,255,255,255,255,255,
    255,253,253,255,255,255,255,255,255,255,255,246,253,253,255,255,255,255,255,255,255,255,252,254,251,254,254,255,255,255,255,255,255,
    255,254,252,255,255,255,255,255,255,255,255,248,254,253,255,255,255,255,255,255,255,255,253,255,254,254,255,255,255,255,255,255,255,
    255,251,254,255,255,255,255,255,255,255,255,245,251,254,255,255,255,255,255,255,255,255,253,253,254,255,255,255,255,255,255,255,255,
    255,251,253,255,255,255,255,255,255,255,255,252,253,254,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,
    255,252,255,255,255,255,255,255,255,255,255,249,255,254,255,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,
    255,255,253,255,255,255,255,255,255,255,255,250,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,254,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};
static const uint8_t kbmode[900] = {
    231,120,48,89,115,113,120,152,112,152,179,64,126,170,118,46,70,95,175,69,143,80,85,82,72,155,103,56,58,10,
    171,218,189,17,13,152,114,26,17,163,44,195,21,10,173,121,24,80,195,26,62,44,64,85,144,71,10,38,171,213,
    144,34,26,170,46,55,19,136,160,33,206,71,63,20,8,114,114,208,12,9,226,81,40,11,96,182,84,29,16,36,
    134,183,89,137,98,101,106,165,148,72,187,100,130,157,111,32,75,80,66,102,167,99,74,62,40,234,128,41,53,9,
    178,241,141,26,8,107,74,43,26,146,73,166,49,23,157,65,38,105,160,51,52,31,115,128,104,79,12,27,217,255,
    87,17,7,87,68,71,44,114,51,15,186,23,47,41,14,110,182,183,21,17,194,66,45,25,102,197,189,23,18,22,
    88,88,147,150,42,46,45,196,205,43,97,183,117,85,38,35,179,61,39,53,200,87,26,21,43,232,171,56,34,51,
    104,114,102,29,93,77,39,28,85,171,58,165,90,98,64,34,22,116,206,23,34,43,166,73,107,54,32,26,51,1,
    81,43,31,68,25,106,22,64,171,36,225,114,34,19,21,102,132,188,16,76,124,62,18,78,95,85,57,50,48,51,
    193,101,35,159,215,111,89,46,111,60,148,31,172,219,228,21,18,111,112,113,77,85,179,255,38,120,114,40,42,1,
    196,245,209,10,25,109,88,43,29,140,166,213,37,43,154,61,63,30,155,67,45,68,1,209,100,80,8,43,154,1,
    51,26,71,142,78,78,16,255,128,34,197,171,41,40,5,102,211,183,4,1,221,51,50,17,168,209,192,23,25,82,
    138,31,36,171,27,166,38,44,229,67,87,58,169,82,115,26,59,179,63,59,90,180,59,166,93,73,154,40,40,21,
    116,143,209,34,39,175,47,15,16,183,34,223,49,45,183,46,17,33,183,6,98,15,32,183,57,46,22,24,128,1,
    54,17,37,65,32,73,115,28,128,23,128,205,40,3,9,115,51,192,18,6,223,87,37,9,115,59,77,64,21,47,
    104,55,44,218,9,54,53,130,226,64,90,70,205,40,41,23,26,57,54,57,112,184,5,41,38,166,213,30,34,26,
    133,152,116,10,32,134,39,19,53,221,26,114,32,73,255,31,9,65,234,2,15,1,118,73,75,32,12,51,192,255,
    160,43,51,88,31,35,67,102,85,55,186,85,56,21,23,111,59,205,45,37,192,55,38,70,124,73,102,1,34,98,
    125,98,42,88,104,85,117,175,82,95,84,53,89,128,100,113,101,45,75,79,123,47,51,128,81,171,1,57,17,5,
    71,102,57,53,41,49,38,33,13,121,57,73,26,1,85,41,10,67,138,77,110,90,47,114,115,21,2,10,102,255,
    166,23,6,101,29,16,10,85,128,101,196,26,57,18,10,102,102,213,34,20,43,117,20,15,36,163,128,68,1,26,
    102,61,71,37,34,53,31,243,192,69,60,71,38,73,119,28,222,37,68,45,128,34,1,47,11,245,171,62,17,19,
    70,146,85,55,62,70,37,43,37,154,100,163,85,160,1,63,9,92,136,28,64,32,201,85,75,15,9,9,64,255,
    184,119,16,86,6,28,5,64,255,25,248,1,56,8,17,132,137,255,55,116,128,58,15,20,82,135,57,26,121,40,
    164,50,31,137,154,133,25,35,218,51,103,44,131,131,123,31,6,158,86,40,64,135,148,224,45,183,128,22,26,17,
    131,240,154,14,1,209,45,16,21,91,64,222,7,1,197,56,21,39,155,60,138,23,102,213,83,12,13,54,192,255,
    68,47,28,85,26,85,85,128,128,32,146,171,18,11,7,63,144,171,4,4,246,35,27,10,146,174,171,12,26,128,
    190,80,35,99,180,80,126,54,45,85,126,47,87,176,51,41,20,32,101,75,128,139,118,146,116,128,85,56,41,15,
    176,236,85,37,9,62,71,30,17,119,118,255,17,18,138,101,38,60,138,55,70,43,26,142,146,36,19,30,171,255,
    97,27,20,138,45,61,62,219,1,81,188,64,32,41,20,117,151,142,20,21,163,112,19,12,61,195,128,48,4,24,
};
static const uint8_t kdc[128] = {
    4,5,6,7,8,9,10,10,11,12,13,14,15,16,17,17,18,19,20,20,21,21,22,22,23,23,24,25,25,26,27,28,
    29,30,31,32,33,34,35,36,37,37,38,39,40,41,42,43,44,45,46,46,47,48,49,50,51,52,53,54,55,56,57,58,
    59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,76,77,78,79,80,81,82,83,84,85,86,87,88,89,
    91,93,95,96,98,100,101,102,104,106,108,110,112,114,116,118,122,124,126,128,130,132,134,136,138,140,143,145,148,151,154,157,
};
static const uint16_t kac[128] = {
    4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,
    24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,
    44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,
    110,112,114,116,119,122,125,128,131,134,137,140,143,146,149,152,155,158,161,164,
    167,170,173,177,181,185,189,193,197,201,205,209,213,217,221,225,229,234,239,245,
    249,254,259,264,269,274,279,284,
};


static inline uint32_t addpx(uint32_t a, uint32_t b) { return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) | (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu); }
static inline uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }
static inline int absi(int v) { return v < 0 ? -v : v; }
static inline int clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ======================= VP8L (lossless) ======================= */
typedef struct { const uint8_t *p, *e; uint32_t acc; int nb, pad; } BR;

static void br_fill(BR *b) {
    while (b->nb <= 24) {
        if (b->p < b->e) b->acc |= (uint32_t)*b->p++ << b->nb; else b->pad++;
        b->nb += 8;
    }
}
static uint32_t gb(BR *b, int n) {              /* n <= 24 */
    if (b->nb < n) br_fill(b);
    uint32_t v = b->acc & ((1u << n) - 1);
    b->acc >>= n; b->nb -= n;
    return v;
}
static int br_eos(const BR *b) { return b->pad * 8 > b->nb; }

#define HB 8
struct huff { uint32_t fast[1 << HB]; uint16_t cnt[16]; uint16_t *sorted; int single; };

static void huff_free(struct huff *h) { free(h->sorted); h->sorted = NULL; }

static int huff_build(struct huff *h, const uint8_t *len, int n) {
    memset(h, 0, sizeof *h);
    int nz = 0, last = 0;
    for (int i = 0; i < n; i++) if (len[i]) { h->cnt[len[i]]++; nz++; last = i; }
    if (!nz) return -1;
    if (nz == 1) { h->single = last + 1; return 0; }          /* one symbol: no bits at all */
    int left = 1;
    for (int l = 1; l < 16; l++) { left = (left << 1) - h->cnt[l]; if (left < 0) return -1; }
    h->sorted = malloc(nz * sizeof *h->sorted);
    if (!h->sorted) return -1;
    uint16_t off[17] = { 0 };
    for (int l = 1; l < 16; l++) off[l + 1] = off[l] + h->cnt[l];
    uint32_t next[17], code = 0;
    for (int l = 1; l < 16; l++) { code = (code + h->cnt[l - 1] * (l > 1)) << 1; next[l] = code; }
    for (int i = 0; i < n; i++) {
        int l = len[i];
        if (!l) continue;
        h->sorted[off[l]++] = i;
        uint32_t c = next[l]++;
        if (l <= HB) {
            uint32_t r = 0;
            for (int k = 0; k < l; k++) r |= ((c >> k) & 1) << (l - 1 - k);
            for (uint32_t k = r; k < (1u << HB); k += 1u << l) h->fast[k] = (uint32_t)l << 16 | i;
        }
    }
    return 0;
}

static int huff_dec(BR *b, const struct huff *h) {
    if (h->single) return h->single - 1;
    if (b->nb < 16) br_fill(b);
    uint32_t e = h->fast[b->acc & ((1 << HB) - 1)];
    if (e) { int l = e >> 16; b->acc >>= l; b->nb -= l; return e & 0xFFFF; }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (b->acc >> (l - 1)) & 1;
        int c = h->cnt[l];
        if (code - c < first) { b->acc >>= l; b->nb -= l; return h->sorted[index + (code - first)]; }
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint8_t kclo[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static int read_code(BR *b, struct huff *h, int n) {
    uint8_t len[2328 + 40];
    memset(len, 0, n);
    if (gb(b, 1)) {                                              /* simple: one or two symbols */
        int two = gb(b, 1) + 1, wide = gb(b, 1);
        int s0 = gb(b, wide ? 8 : 1);
        if (s0 >= n) return -1;
        len[s0] = 1;
        if (two == 2) { int s1 = gb(b, 8); if (s1 >= n) return -1; len[s1] = 1; }
    } else {
        uint8_t cl[19] = { 0 };
        int nc = 4 + gb(b, 4);
        for (int i = 0; i < nc; i++) cl[kclo[i]] = gb(b, 3);
        struct huff ch;
        if (huff_build(&ch, cl, 19) < 0) return -1;
        int maxsym = n;
        if (gb(b, 1)) {
            int nbits = 2 + 2 * gb(b, 3);
            maxsym = 2 + gb(b, nbits);
            if (maxsym > n) { huff_free(&ch); return -1; }
        }
        int sym = 0, prev = 8;
        while (sym < n && maxsym-- > 0) {
            int c = huff_dec(b, &ch);
            if (c < 0 || br_eos(b)) { huff_free(&ch); return -1; }
            if (c < 16) { len[sym++] = c; if (c) prev = c; }
            else {
                int extra = c == 16 ? 2 : c == 17 ? 3 : 7, rep = (c == 18 ? 11 : 3) + gb(b, extra);
                if (sym + rep > n) { huff_free(&ch); return -1; }
                int v = c == 16 ? prev : 0;
                while (rep--) len[sym++] = v;
            }
        }
        huff_free(&ch);
    }
    if (br_eos(b)) return -1;
    return huff_build(h, len, n);
}

struct xf { int type, bits, xs, ys; uint32_t *data; };       /* 0 predictor, 1 cross-colour, 2 subtract green, 3 colour indexing */

static int sub(int v, int bits) { return (v + (1 << bits) - 1) >> bits; }

static const int8_t kdist[120][2] = {
    {0,1},{1,0},{1,1},{-1,1},{0,2},{2,0},{1,2},{-1,2},{2,1},{-2,1},{2,2},{-2,2},{0,3},{3,0},{1,3},{-1,3},{3,1},{-3,1},{2,3},{-2,3},
    {3,2},{-3,2},{0,4},{4,0},{1,4},{-1,4},{4,1},{-4,1},{3,3},{-3,3},{2,4},{-2,4},{4,2},{-4,2},{0,5},{3,4},{-3,4},{4,3},{-4,3},{5,0},
    {1,5},{-1,5},{5,1},{-5,1},{2,5},{-2,5},{5,2},{-5,2},{4,4},{-4,4},{3,5},{-3,5},{5,3},{-5,3},{0,6},{6,0},{1,6},{-1,6},{6,1},{-6,1},
    {2,6},{-2,6},{6,2},{-6,2},{4,5},{-4,5},{5,4},{-5,4},{3,6},{-3,6},{6,3},{-6,3},{0,7},{7,0},{1,7},{-1,7},{5,5},{-5,5},{7,1},{-7,1},
    {4,6},{-4,6},{6,4},{-6,4},{2,7},{-2,7},{7,2},{-7,2},{3,7},{-3,7},{7,3},{-7,3},{5,6},{-5,6},{6,5},{-6,5},{8,0},{4,7},{-4,7},{7,4},
    {-7,4},{8,1},{8,2},{6,6},{-6,6},{8,3},{5,7},{-5,7},{7,5},{-7,5},{8,4},{6,7},{-6,7},{7,6},{-7,6},{8,5},{7,7},{-7,7},{8,6},{8,7} };

static int pfx_val(BR *b, int sym) {                          /* LZ77 length / distance prefix symbol -> value */
    if (sym < 4) return sym + 1;
    int eb = (sym - 2) >> 1, off = (2 + (sym & 1)) << eb;
    return off + gb(b, eb) + 1;
}

static uint32_t *vl_image(BR *b, int xs, int ys, int top, struct xf *tr, int *ntr, int *fxs);

/* decode the entropy-coded pixels of one (sub-)image: xs x ys ARGB */
static uint32_t *vl_pixels(BR *b, int xs, int ys, int top) {
    int cbits = 0, hb = 0, mw = 0, ngroup = 1;
    uint32_t *meta = NULL, *cache = NULL, *out = NULL;
    struct huff *hg = NULL;
    if (gb(b, 1)) { cbits = gb(b, 4); if (cbits < 1 || cbits > 11) return NULL; }
    if (top && gb(b, 1)) {
        struct xf dummy[4]; int nd = 0, fx;
        hb = gb(b, 3) + 2; mw = sub(xs, hb);
        meta = vl_image(b, mw, sub(ys, hb), 0, dummy, &nd, &fx);
        if (!meta) return NULL;
        int mx = 0;
        for (int i = 0; i < mw * sub(ys, hb); i++) { meta[i] = (meta[i] >> 8) & 0xFFFF; if ((int)meta[i] > mx) mx = meta[i]; }
        ngroup = mx + 1;
        if (ngroup > 16384) goto fail;
    }
    hg = calloc(ngroup * 5, sizeof *hg);
    if (!hg) goto fail;
    for (int g = 0; g < ngroup; g++) {
        static const int base[5] = { 256 + 24, 256, 256, 256, 40 };
        for (int k = 0; k < 5; k++)
            if (read_code(b, &hg[g * 5 + k], base[k] + (k == 0 && cbits ? 1 << cbits : 0)) < 0) goto fail;
    }
    if (cbits) { cache = calloc(1u << cbits, 4); if (!cache) goto fail; }
    long total = (long)xs * ys;
    out = malloc(total * 4 + 4);
    if (!out) goto fail;
    long pos = 0;
    int x = 0, y = 0;
    while (pos < total) {
        const struct huff *h = &hg[(meta ? meta[(y >> hb) * mw + (x >> hb)] : 0) * 5];
        int s = huff_dec(b, &h[0]);
        if (s < 0 || br_eos(b)) goto fail;
        if (s < 256) {
            int r = huff_dec(b, &h[1]), bl = huff_dec(b, &h[2]), a = huff_dec(b, &h[3]);
            if (r < 0 || bl < 0 || a < 0) goto fail;
            uint32_t px = (uint32_t)a << 24 | r << 16 | s << 8 | bl;
            out[pos++] = px;
            if (cache) cache[(0x1e35a7bdu * px) >> (32 - cbits)] = px;
            if (++x == xs) { x = 0; y++; }
        } else if (s < 280) {
            int len = pfx_val(b, s - 256), ds = huff_dec(b, &h[4]);
            if (ds < 0) goto fail;
            int dc = pfx_val(b, ds), dist;
            if (dc > 120) dist = dc - 120;
            else { dist = kdist[dc - 1][1] * xs + kdist[dc - 1][0]; if (dist < 1) dist = 1; }
            if (dist > pos || pos + len > total) goto fail;
            for (int i = 0; i < len; i++) {
                uint32_t px = out[pos - dist];
                out[pos++] = px;
                if (cache) cache[(0x1e35a7bdu * px) >> (32 - cbits)] = px;
                if (++x == xs) { x = 0; y++; }
            }
        } else {
            int ci = s - 280;
            if (!cache || ci >= (1 << cbits)) goto fail;
            uint32_t px = cache[ci];
            out[pos++] = px;
            if (++x == xs) { x = 0; y++; }
        }
    }
    for (int i = 0; i < ngroup * 5; i++) huff_free(&hg[i]);
    free(hg); free(meta); free(cache);
    return out;
fail:
    if (hg) for (int i = 0; i < ngroup * 5; i++) huff_free(&hg[i]);
    free(hg); free(meta); free(cache); free(out);
    return NULL;
}

static uint32_t *vl_image(BR *b, int xs, int ys, int top, struct xf *tr, int *ntr, int *fxs) {
    *ntr = 0;
    if (top) {
        int seen = 0;
        while (gb(b, 1)) {
            int t = gb(b, 2);
            if (seen & 1 << t || *ntr >= 4) goto bad;
            seen |= 1 << t;
            struct xf *x = &tr[(*ntr)++];
            *x = (struct xf){ t, 0, xs, ys, NULL };
            if (t == 0 || t == 1) {
                x->bits = gb(b, 3) + 2;
                int d0, d1;
                x->data = vl_image(b, sub(xs, x->bits), sub(ys, x->bits), 0, tr + 4, &d0, &d1);   /* tr + 4: scratch, unused */
                if (!x->data) goto bad;
            } else if (t == 3) {
                int nc = gb(b, 8) + 1, d0, d1;
                x->bits = nc <= 2 ? 3 : nc <= 4 ? 2 : nc <= 16 ? 1 : 0;
                uint32_t *p = vl_image(b, nc, 1, 0, tr + 4, &d0, &d1);
                if (!p) goto bad;
                x->data = calloc(256, 4);
                if (!x->data) { free(p); goto bad; }
                for (int i = 0; i < nc; i++) x->data[i] = i ? addpx(x->data[i - 1], p[i]) : p[i];
                free(p);
                xs = sub(xs, x->bits);
            }
        }
    }
    *fxs = xs;
    uint32_t *r = vl_pixels(b, xs, ys, top);
    if (r) return r;
bad:
    for (int i = 0; i < *ntr; i++) free(tr[i].data);
    *ntr = 0;
    return NULL;
}


static uint32_t predict(int m, uint32_t L, uint32_t T, uint32_t TR, uint32_t TL) {
    switch (m) {
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: return avg2(avg2(L, TR), T);
    case 6: return avg2(L, TL);
    case 7: return avg2(L, T);
    case 8: return avg2(TL, T);
    case 9: return avg2(T, TR);
    case 10: return avg2(avg2(L, TL), avg2(T, TR));
    case 11: {
        int pl = 0, pt = 0;
        for (int s = 0; s < 32; s += 8) {
            int l = (L >> s) & 255, t = (T >> s) & 255, tl = (TL >> s) & 255, p = l + t - tl;
            pl += absi(p - l); pt += absi(p - t);
        }
        return pl < pt ? L : T;
    }
    case 12: case 13: {
        uint32_t o = 0, a = m == 13 ? avg2(L, T) : L;
        for (int s = 0; s < 32; s += 8) {
            int c0 = (a >> s) & 255, c1 = m == 13 ? (TL >> s) & 255 : (T >> s) & 255, c2 = (TL >> s) & 255;
            o |= (uint32_t)clip8(m == 13 ? c0 + (c0 - c1) / 2 : c0 + c1 - c2) << s;
        }
        return o;
    }
    default: return 0xFF000000u;
    }
}

/* undo the transforms of one image in place (colour indexing may return a new, wider buffer); NULL on OOM */
static uint32_t *vl_inverse(uint32_t *px, struct xf *tr, int n, int ys) {
    for (int t = n - 1; t >= 0; t--) {
        struct xf *x = &tr[t];
        int xs = x->xs;
        if (x->type == 0) {
            int tw = sub(xs, x->bits);
            for (int y = 0; y < ys; y++) {
                uint32_t *r = px + (long)y * xs;
                for (int c = 0; c < xs; c++) {
                    uint32_t pr;
                    if (y == 0) pr = c ? r[c - 1] : 0xFF000000u;
                    else if (c == 0) pr = r[c - xs];
                    else {
                        int m = (x->data[(y >> x->bits) * tw + (c >> x->bits)] >> 8) & 15;
                        pr = predict(m, r[c - 1], r[c - xs], r[c - xs + 1], r[c - xs - 1]);
                    }
                    r[c] = addpx(r[c], pr);
                }
            }
        } else if (x->type == 1) {
            int tw = sub(xs, x->bits);
            for (int y = 0; y < ys; y++) {
                uint32_t *r = px + (long)y * xs;
                for (int c = 0; c < xs; c++) {
                    uint32_t k = x->data[(y >> x->bits) * tw + (c >> x->bits)], p = r[c];
                    int g2r = (int8_t)(k & 255), g2b = (int8_t)((k >> 8) & 255), r2b = (int8_t)((k >> 16) & 255);
                    int g = (int8_t)((p >> 8) & 255), red = (p >> 16) & 255, bl = p & 255;
                    red = (red + ((g2r * g) >> 5)) & 255;
                    bl = (bl + ((g2b * g) >> 5) + ((r2b * (int8_t)red) >> 5)) & 255;
                    r[c] = (p & 0xFF00FF00u) | red << 16 | bl;
                }
            }
        } else if (x->type == 2) {
            for (long i = 0; i < (long)xs * ys; i++) {
                uint32_t p = px[i], g = (p >> 8) & 255;
                px[i] = (p & 0xFF00FF00u) | ((((p >> 16) + g) & 255) << 16) | ((p + g) & 255);
            }
        } else {
            int pxs = sub(xs, x->bits), bpp = 8 >> x->bits, mask = (1 << bpp) - 1;
            uint32_t *o = malloc((long)xs * ys * 4 + 4);
            if (!o) { free(px); return NULL; }
            for (int y = 0; y < ys; y++)
                for (int c = 0; c < xs; c++) {
                    uint32_t g = (px[(long)y * pxs + (c >> x->bits)] >> 8) & 255;
                    o[(long)y * xs + c] = x->data[x->bits ? (g >> (bpp * (c & ((1 << x->bits) - 1)))) & mask : g];
                }
            free(px);
            px = o;
        }
    }
    return px;
}

/* a VP8L stream (with its 5-byte header unless alpha_plane says the size is known): ARGB, w*h */
static uint32_t *vl_decode(const uint8_t *d, size_t n, int *w, int *h, int headless) {
    BR b = { d, d + n, 0, 0, 0 };
    if (!headless) {
        if (gb(&b, 8) != 0x2F) return NULL;
        *w = gb(&b, 14) + 1; *h = gb(&b, 14) + 1;
        gb(&b, 1);
        if (gb(&b, 3) != 0) return NULL;
    }
    if ((unsigned)*w * *h > WEBP_MAXPIX) return NULL;
    struct xf tr[8];
    int nt, fx;
    uint32_t *px = vl_image(&b, *w, *h, 1, tr, &nt, &fx);
    if (!px) return NULL;
    px = vl_inverse(px, tr, nt, *h);
    for (int i = 0; i < nt; i++) free(tr[i].data);
    return px;
}

/* ======================= VP8 (lossy) ======================= */
typedef struct { const uint8_t *p, *e; uint32_t value, range; int bc; } BD;

static inline int bd_byte(BD *d) { return d->p < d->e ? *d->p++ : 0; }
static void bd_init(BD *d, const uint8_t *p, size_t n) {
    d->p = p; d->e = p + n; d->range = 255; d->bc = 0;
    d->value = bd_byte(d) << 8; d->value |= bd_byte(d);
}
static int bd_get(BD *d, int prob) {
    uint32_t split = 1 + (((d->range - 1) * prob) >> 8), big = split << 8;
    int bit;
    if (d->value >= big) { bit = 1; d->range -= split; d->value -= big; } else { bit = 0; d->range = split; }
    while (d->range < 128) {
        d->value <<= 1; d->range <<= 1;
        if (++d->bc == 8) { d->bc = 0; d->value |= bd_byte(d); }
    }
    return bit;
}
static int bd_val(BD *d, int bits) { int v = 0; while (bits--) v = v << 1 | bd_get(d, 128); return v; }
static int bd_sval(BD *d, int bits) { int v = bd_val(d, bits); return bd_get(d, 128) ? -v : v; }

static const uint8_t kzig[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t kband[17] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0 };
static const uint8_t kcat3[] = { 173, 148, 140, 0 }, kcat4[] = { 176, 155, 140, 135, 0 }, kcat5[] = { 180, 157, 141, 134, 130, 0 },
                     kcat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 };
static const uint8_t *const kcat[4] = { kcat3, kcat4, kcat5, kcat6 };
static const int8_t kintra4[18] = { 0, 1, -1, 2, -2, 3, 4, 6, -3, 5, -4, -5, -6, 7, -7, 8, -8, -9 };   /* tree; leaf l is stored as -l, B_DC_PRED (0) as 0 */

enum { B_DC, B_TM, B_VE, B_HE, B_RD, B_VR, B_LD, B_VL, B_HD, B_HU, B_PRED };

struct mbi { uint8_t i4, ymode, uvmode, seg, skip, bm[16]; };
struct finfo { uint8_t limit, ilevel, hev, inner; };

struct vp8 {
    BD br, tok[8];
    int nparts, w, h, mbw, mbh;
    int use_seg, upd_map, abs_delta, seg_q[4], seg_f[4]; uint8_t seg_p[3];
    int simple, flevel, sharp, use_lfd, ref_d[4], mode_d[4];
    int q0, dq[5];
    int qm[4][6];                                           /* y1 dc/ac, y2 dc/ac, uv dc/ac */
    uint8_t probs[4][8][3][11];
    int use_skip, skip_p;
    uint8_t *Yb, *Ub, *Vb, *Y, *U, *V;
    int ys, uvs;
    struct finfo fs[4][2];
};

static int large(BD *br, const uint8_t *p) {
    int v;
    if (!bd_get(br, p[3])) { if (!bd_get(br, p[4])) v = 2; else v = 3 + bd_get(br, p[5]); }
    else if (!bd_get(br, p[6])) {
        if (!bd_get(br, p[7])) v = 5 + bd_get(br, 159);
        else { v = 7 + 2 * bd_get(br, 165); v += bd_get(br, 145); }
    } else {
        int b1 = bd_get(br, p[8]), b0 = bd_get(br, p[9 + b1]), cat = 2 * b1 + b0;
        v = 0;
        for (const uint8_t *t = kcat[cat]; *t; t++) v += v + bd_get(br, *t);
        v += 3 + (8 << cat);
    }
    return v;
}

static int getcoeffs(struct vp8 *s, BD *br, int type, int ctx, const int *dq, int n, int16_t *out) {
    const uint8_t (*pr)[3][11] = s->probs[type];
    const uint8_t *p = pr[kband[n]][ctx];
    for (; n < 16; n++) {
        if (!bd_get(br, p[0])) return n;
        while (!bd_get(br, p[1])) { if (++n == 16) return 16; p = pr[kband[n]][0]; }
        int v;
        const uint8_t (*pn)[11] = pr[kband[n + 1]];
        if (!bd_get(br, p[2])) { v = 1; p = pn[1]; } else { v = large(br, p); p = pn[2]; }
        out[kzig[n]] = (bd_get(br, 128) ? -v : v) * dq[n > 0];
    }
    return 16;
}

static void wht(const int16_t *in, int16_t *out) {
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[i] + in[12 + i], a1 = in[4 + i] + in[8 + i], a2 = in[4 + i] - in[8 + i], a3 = in[i] - in[12 + i];
        tmp[i] = a0 + a1; tmp[8 + i] = a0 - a1; tmp[4 + i] = a3 + a2; tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[i * 4] + 3, a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4], a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[0] = (a0 + a1) >> 3; out[16] = (a3 + a2) >> 3; out[32] = (a0 - a1) >> 3; out[48] = (a3 - a2) >> 3;
        out += 64;
    }
}

#define MUL1(a) ((((a) * 20091) >> 16) + (a))
#define MUL2(a) (((a) * 35468) >> 16)
static void idct_add(const int16_t *in, uint8_t *dst, int st, int dc_only) {
    if (dc_only) {
        int dc = (in[0] + 4) >> 3;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) dst[y * st + x] = clip8(dst[y * st + x] + dc);
        return;
    }
    int C[16], *t = C;
    for (int i = 0; i < 4; i++) {
        int a = in[0] + in[8], b = in[0] - in[8], c = MUL2(in[4]) - MUL1(in[12]), d = MUL1(in[4]) + MUL2(in[12]);
        t[0] = a + d; t[1] = b + c; t[2] = b - c; t[3] = a - d;
        t += 4; in++;
    }
    t = C;
    for (int i = 0; i < 4; i++) {
        int dc = t[0] + 4, a = dc + t[8], b = dc - t[8], c = MUL2(t[4]) - MUL1(t[12]), d = MUL1(t[4]) + MUL2(t[12]);
        dst[0] = clip8(dst[0] + ((a + d) >> 3)); dst[1] = clip8(dst[1] + ((b + c) >> 3));
        dst[2] = clip8(dst[2] + ((b - c) >> 3)); dst[3] = clip8(dst[3] + ((a - d) >> 3));
        t++; dst += st;
    }
}

static void predn(uint8_t *d, int st, int n, int mode, int has_top, int has_left) {
    int lg = n == 16 ? 4 : 3;
    if (mode == B_DC) {
        int s = 0, v = 128;
        if (has_top) for (int i = 0; i < n; i++) s += d[i - st];
        if (has_left) for (int i = 0; i < n; i++) s += d[i * st - 1];
        if (has_top && has_left) v = (s + n) >> (lg + 1);
        else if (has_top || has_left) v = (s + n / 2) >> lg;
        for (int y = 0; y < n; y++) memset(d + y * st, v, n);
    } else if (mode == B_TM) {
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) d[y * st + x] = clip8(d[x - st] + d[y * st - 1] - d[-st - 1]);
    } else if (mode == B_VE) {
        for (int y = 0; y < n; y++) memcpy(d + y * st, d - st, n);
    } else {
        for (int y = 0; y < n; y++) memset(d + y * st, d[y * st - 1], n);
    }
}

#define AVG3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (((a) + (b) + 1) >> 1)
static void pred4(uint8_t *d, int st, int mode, const uint8_t *A, const uint8_t *Lf, int X) {
    #define DST(x, y) d[(x) + (y) * st]
    int I = Lf[0], J = Lf[1], K = Lf[2], L = Lf[3];
    int a = A[0], b = A[1], c = A[2], dd = A[3], e = A[4], f = A[5], g = A[6], h = A[7];
    switch (mode) {
    case B_DC: { int s = 4; for (int i = 0; i < 4; i++) s += A[i] + Lf[i]; s >>= 3; for (int y = 0; y < 4; y++) memset(d + y * st, s, 4); break; }
    case B_TM: for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) DST(x, y) = clip8(A[x] + Lf[y] - X); break;
    case B_VE: { uint8_t v[4] = { AVG3(X, a, b), AVG3(a, b, c), AVG3(b, c, dd), AVG3(c, dd, e) }; for (int y = 0; y < 4; y++) memcpy(d + y * st, v, 4); break; }
    case B_HE: {
        uint8_t v[4] = { AVG3(X, I, J), AVG3(I, J, K), AVG3(J, K, L), AVG3(K, L, L) };
        for (int y = 0; y < 4; y++) memset(d + y * st, v[y], 4);
        break; }
    case B_RD:
        DST(0, 3) = AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(a, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(b, a, X);
        DST(3, 1) = DST(2, 0) = AVG3(c, b, a);
        DST(3, 0) = AVG3(dd, c, b);
        break;
    case B_VR:
        DST(0, 0) = DST(1, 2) = AVG2(X, a);
        DST(1, 0) = DST(2, 2) = AVG2(a, b);
        DST(2, 0) = DST(3, 2) = AVG2(b, c);
        DST(3, 0) = AVG2(c, dd);
        DST(0, 3) = AVG3(K, J, I);
        DST(0, 2) = AVG3(J, I, X);
        DST(0, 1) = DST(1, 3) = AVG3(I, X, a);
        DST(1, 1) = DST(2, 3) = AVG3(X, a, b);
        DST(2, 1) = DST(3, 3) = AVG3(a, b, c);
        DST(3, 1) = AVG3(b, c, dd);
        break;
    case B_LD:
        DST(0, 0) = AVG3(a, b, c);
        DST(1, 0) = DST(0, 1) = AVG3(b, c, dd);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(c, dd, e);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(dd, e, f);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(e, f, g);
        DST(3, 2) = DST(2, 3) = AVG3(f, g, h);
        DST(3, 3) = AVG3(g, h, h);
        break;
    case B_VL:
        DST(0, 0) = AVG2(a, b);
        DST(1, 0) = DST(0, 2) = AVG2(b, c);
        DST(2, 0) = DST(1, 2) = AVG2(c, dd);
        DST(3, 0) = DST(2, 2) = AVG2(dd, e);
        DST(0, 1) = AVG3(a, b, c);
        DST(1, 1) = DST(0, 3) = AVG3(b, c, dd);
        DST(2, 1) = DST(1, 3) = AVG3(c, dd, e);
        DST(3, 1) = DST(2, 3) = AVG3(dd, e, f);
        DST(3, 2) = AVG3(e, f, g);
        DST(3, 3) = AVG3(f, g, h);
        break;
    case B_HD:
        DST(0, 0) = DST(2, 1) = AVG2(I, X);
        DST(0, 1) = DST(2, 2) = AVG2(J, I);
        DST(0, 2) = DST(2, 3) = AVG2(K, J);
        DST(0, 3) = AVG2(L, K);
        DST(3, 0) = AVG3(a, b, c);
        DST(2, 0) = AVG3(X, a, b);
        DST(1, 0) = DST(3, 1) = AVG3(I, X, a);
        DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
        DST(1, 3) = AVG3(L, K, J);
        break;
    default: /* B_HU */
        DST(0, 0) = AVG2(I, J);
        DST(2, 0) = DST(0, 1) = AVG2(J, K);
        DST(2, 1) = DST(0, 2) = AVG2(K, L);
        DST(1, 0) = AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
        break;
    }
    #undef DST
}

/* ---- loop filter ---- */
static inline int sc1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sc2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }
static inline void filt2(uint8_t *p, int st) {
    int p1 = p[-2 * st], p0 = p[-st], q0 = p[0], q1 = p[st];
    int a = 3 * (q0 - p0) + sc1(p1 - q1), a1 = sc2((a + 4) >> 3), a2 = sc2((a + 3) >> 3);
    p[-st] = clip8(p0 + a2); p[0] = clip8(q0 - a1);
}
static inline void filt4(uint8_t *p, int st) {
    int p1 = p[-2 * st], p0 = p[-st], q0 = p[0], q1 = p[st];
    int a = 3 * (q0 - p0), a1 = sc2((a + 4) >> 3), a2 = sc2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * st] = clip8(p1 + a3); p[-st] = clip8(p0 + a2); p[0] = clip8(q0 - a1); p[st] = clip8(q1 - a3);
}
static inline void filt6(uint8_t *p, int st) {
    int p2 = p[-3 * st], p1 = p[-2 * st], p0 = p[-st], q0 = p[0], q1 = p[st], q2 = p[2 * st];
    int a = sc1(3 * (q0 - p0) + sc1(p1 - q1)), a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * st] = clip8(p2 + a3); p[-2 * st] = clip8(p1 + a2); p[-st] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1); p[st] = clip8(q1 - a2); p[2 * st] = clip8(q2 - a3);
}
static inline int hev(const uint8_t *p, int st, int t) { return absi(p[-2 * st] - p[-st]) > t || absi(p[st] - p[0]) > t; }
static inline int need1(const uint8_t *p, int st, int t) { return 4 * absi(p[-st] - p[0]) + absi(p[-2 * st] - p[st]) <= t; }
static inline int need2(const uint8_t *p, int st, int t, int it) {
    if (4 * absi(p[-st] - p[0]) + absi(p[-2 * st] - p[st]) > t) return 0;
    return absi(p[-4 * st] - p[-3 * st]) <= it && absi(p[-3 * st] - p[-2 * st]) <= it && absi(p[-2 * st] - p[-st]) <= it &&
           absi(p[3 * st] - p[2 * st]) <= it && absi(p[2 * st] - p[st]) <= it && absi(p[st] - p[0]) <= it;
}
static void simple_edge(uint8_t *p, int hs, int vs, int size, int thresh) {
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < size; i++, p += vs) if (need1(p, hs, t2)) filt2(p, hs);
}
static void loop_edge(uint8_t *p, int hs, int vs, int size, int thresh, int it, int ht, int mb) {
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < size; i++, p += vs)
        if (need2(p, hs, t2, it)) { if (hev(p, hs, ht)) filt2(p, hs); else if (mb) filt6(p, hs); else filt4(p, hs); }
}

static void filter_frame(struct vp8 *s, const struct finfo *fi) {
    for (int my = 0; my < s->mbh; my++)
        for (int mx = 0; mx < s->mbw; mx++) {
            const struct finfo *f = &fi[my * s->mbw + mx];
            if (!f->limit) continue;
            int ys = s->ys, uvs = s->uvs, lim = f->limit, il = f->ilevel, ht = f->hev;
            uint8_t *y = s->Y + my * 16 * ys + mx * 16, *u = s->U + my * 8 * uvs + mx * 8, *v = s->V + my * 8 * uvs + mx * 8;
            if (s->simple) {
                if (mx > 0) simple_edge(y, 1, ys, 16, lim + 4);
                if (f->inner) for (int k = 4; k < 16; k += 4) simple_edge(y + k, 1, ys, 16, lim);
                if (my > 0) simple_edge(y, ys, 1, 16, lim + 4);
                if (f->inner) for (int k = 4; k < 16; k += 4) simple_edge(y + k * ys, ys, 1, 16, lim);
            } else {
                if (mx > 0) { loop_edge(y, 1, ys, 16, lim + 4, il, ht, 1); loop_edge(u, 1, uvs, 8, lim + 4, il, ht, 1); loop_edge(v, 1, uvs, 8, lim + 4, il, ht, 1); }
                if (f->inner) {
                    for (int k = 4; k < 16; k += 4) loop_edge(y + k, 1, ys, 16, lim, il, ht, 0);
                    loop_edge(u + 4, 1, uvs, 8, lim, il, ht, 0); loop_edge(v + 4, 1, uvs, 8, lim, il, ht, 0);
                }
                if (my > 0) { loop_edge(y, ys, 1, 16, lim + 4, il, ht, 1); loop_edge(u, uvs, 1, 8, lim + 4, il, ht, 1); loop_edge(v, uvs, 1, 8, lim + 4, il, ht, 1); }
                if (f->inner) {
                    for (int k = 4; k < 16; k += 4) loop_edge(y + k * ys, ys, 1, 16, lim, il, ht, 0);
                    loop_edge(u + 4 * uvs, uvs, 1, 8, lim, il, ht, 0); loop_edge(v + 4 * uvs, uvs, 1, 8, lim, il, ht, 0);
                }
            }
        }
}

/* ---- colour conversion + fancy upsampling (libwebp's) ---- */
#define MH(v, c) (((v) * (c)) >> 8)
static inline int c6(int v) { return (v & ~(256 * 64 - 1)) == 0 ? v >> 6 : v < 0 ? 0 : 255; }
static inline uint32_t yuv(int y, int u, int v) {
    int r = c6(MH(y, 19077) + MH(v, 26149) - 14234), g = c6(MH(y, 19077) - MH(u, 6419) - MH(v, 13320) + 8708), b = c6(MH(y, 19077) + MH(u, 33050) - 17685);
    return 0xFF000000u | r << 16 | g << 8 | b;
}
#define LUV(u, v) ((uint32_t)(u) | (uint32_t)(v) << 16)
static void up_pair(const uint8_t *ty, const uint8_t *by, const uint8_t *tu, const uint8_t *tv, const uint8_t *cu, const uint8_t *cv,
                    uint32_t *td, uint32_t *bd, int len) {
    int last = (len - 1) >> 1;
    uint32_t tl = LUV(tu[0], tv[0]), l = LUV(cu[0], cv[0]);
    uint32_t uv0 = (3 * tl + l + 0x00020002u) >> 2;
    td[0] = yuv(ty[0], uv0 & 255, uv0 >> 16);
    if (by) { uv0 = (3 * l + tl + 0x00020002u) >> 2; bd[0] = yuv(by[0], uv0 & 255, uv0 >> 16); }
    for (int x = 1; x <= last; x++) {
        uint32_t t = LUV(tu[x], tv[x]), uv = LUV(cu[x], cv[x]);
        uint32_t avg = tl + t + l + uv + 0x00080008u, d12 = (avg + 2 * (t + l)) >> 3, d03 = (avg + 2 * (tl + uv)) >> 3;
        uint32_t a = (d12 + tl) >> 1, b = (d03 + t) >> 1;
        td[2 * x - 1] = yuv(ty[2 * x - 1], a & 255, a >> 16);
        td[2 * x] = yuv(ty[2 * x], b & 255, b >> 16);
        if (by) {
            a = (d03 + l) >> 1; b = (d12 + uv) >> 1;
            bd[2 * x - 1] = yuv(by[2 * x - 1], a & 255, a >> 16);
            bd[2 * x] = yuv(by[2 * x], b & 255, b >> 16);
        }
        tl = t; l = uv;
    }
    if (!(len & 1)) {
        uv0 = (3 * tl + l + 0x00020002u) >> 2;
        td[len - 1] = yuv(ty[len - 1], uv0 & 255, uv0 >> 16);
        if (by) { uv0 = (3 * l + tl + 0x00020002u) >> 2; bd[len - 1] = yuv(by[len - 1], uv0 & 255, uv0 >> 16); }
    }
}

static uint32_t *vp8_decode(const uint8_t *d, size_t n, int *ow, int *oh) {
    if (n < 10 || (d[0] & 1) || memcmp(d + 3, "\x9d\x01\x2a", 3)) return NULL;       /* key frames only */
    uint32_t tag = d[0] | d[1] << 8 | d[2] << 16;
    size_t fp = tag >> 5;
    int w = (d[6] | d[7] << 8) & 0x3FFF, h = (d[8] | d[9] << 8) & 0x3FFF;
    if (!w || !h || fp > n - 10 || (unsigned)w * h > WEBP_MAXPIX) return NULL;
    struct vp8 *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    uint32_t *out = NULL;
    uint8_t *ba = NULL, *topn = NULL, *aboveb = NULL;
    struct mbi *mbs = NULL;
    struct finfo *fi = NULL;
    s->w = w; s->h = h; s->mbw = (w + 15) >> 4; s->mbh = (h + 15) >> 4;
    BD *br = &s->br;
    bd_init(br, d + 10, fp);
    bd_val(br, 2);                                                    /* colour space, clamp type */
    s->use_seg = bd_val(br, 1);
    if (s->use_seg) {
        s->upd_map = bd_val(br, 1);
        if (bd_val(br, 1)) {
            s->abs_delta = bd_val(br, 1);
            for (int i = 0; i < 4; i++) s->seg_q[i] = bd_val(br, 1) ? bd_sval(br, 7) : 0;
            for (int i = 0; i < 4; i++) s->seg_f[i] = bd_val(br, 1) ? bd_sval(br, 6) : 0;
        }
        if (s->upd_map) for (int i = 0; i < 3; i++) s->seg_p[i] = bd_val(br, 1) ? bd_val(br, 8) : 255;
    }
    s->simple = bd_val(br, 1); s->flevel = bd_val(br, 6); s->sharp = bd_val(br, 3);
    s->use_lfd = bd_val(br, 1);
    if (s->use_lfd && bd_val(br, 1)) {
        for (int i = 0; i < 4; i++) if (bd_val(br, 1)) s->ref_d[i] = bd_sval(br, 6);
        for (int i = 0; i < 4; i++) if (bd_val(br, 1)) s->mode_d[i] = bd_sval(br, 6);
    }
    /* token partitions */
    s->nparts = 1 << bd_val(br, 2);
    {
        const uint8_t *sz = d + 10 + fp, *end = d + n;
        size_t left = (size_t)(end - sz), nt = s->nparts - 1;
        if (left < 3 * nt) goto done;
        const uint8_t *ps = sz + 3 * nt;
        left -= 3 * nt;
        for (int i = 0; i < (int)nt; i++, sz += 3) {
            size_t pl = sz[0] | sz[1] << 8 | sz[2] << 16;
            if (pl > left) pl = left;
            bd_init(&s->tok[i], ps, pl);
            ps += pl; left -= pl;
        }
        bd_init(&s->tok[nt], ps, left);
    }
    /* quantisers */
    s->q0 = bd_val(br, 7);
    for (int i = 0; i < 5; i++) s->dq[i] = bd_val(br, 1) ? bd_sval(br, 4) : 0;       /* y1dc y2dc y2ac uvdc uvac */
    for (int sg = 0; sg < 4; sg++) {
        int q = s->use_seg ? (s->abs_delta ? s->seg_q[sg] : s->seg_q[sg] + s->q0) : s->q0;
        #define QI(x) ((x) < 0 ? 0 : (x) > 127 ? 127 : (x))
        s->qm[sg][0] = kdc[QI(q + s->dq[0])]; s->qm[sg][1] = kac[QI(q)];
        s->qm[sg][2] = kdc[QI(q + s->dq[1])] * 2; s->qm[sg][3] = kac[QI(q + s->dq[2])] * 155 / 100;
        if (s->qm[sg][3] < 8) s->qm[sg][3] = 8;
        s->qm[sg][4] = kdc[QI(q + s->dq[3])]; if (s->qm[sg][4] > 132) s->qm[sg][4] = 132;
        s->qm[sg][5] = kac[QI(q + s->dq[4])];
    }
    bd_val(br, 1);                                                    /* refresh_entropy_probs */
    for (int t = 0; t < 4; t++) for (int b = 0; b < 8; b++) for (int c = 0; c < 3; c++) for (int p = 0; p < 11; p++) {
        int i = ((t * 8 + b) * 3 + c) * 11 + p;
        s->probs[t][b][c][p] = bd_get(br, kupd[i]) ? bd_val(br, 8) : kcoef[i];
    }
    s->use_skip = bd_val(br, 1);
    if (s->use_skip) s->skip_p = bd_val(br, 8);
    /* loop filter strengths per segment / mode */
    int ftype = s->flevel == 0 ? 0 : s->simple ? 1 : 2;
    for (int sg = 0; sg < 4; sg++) {
        int base = s->use_seg ? (s->abs_delta ? s->seg_f[sg] : s->seg_f[sg] + s->flevel) : s->flevel;
        for (int i4 = 0; i4 < 2; i4++) {
            int lv = base;
            if (s->use_lfd) { lv += s->ref_d[0]; if (i4) lv += s->mode_d[0]; }
            lv = lv < 0 ? 0 : lv > 63 ? 63 : lv;
            struct finfo *f = &s->fs[sg][i4];
            if (lv > 0) {
                int il = lv;
                if (s->sharp > 0) { il >>= s->sharp > 4 ? 2 : 1; if (il > 9 - s->sharp) il = 9 - s->sharp; }
                if (il < 1) il = 1;
                f->ilevel = il; f->limit = 2 * lv + il; f->hev = lv >= 40 ? 2 : lv >= 15 ? 1 : 0;
            }
            f->inner = i4;
        }
    }
    /* planes with a 1-pixel top/left guard (127 above, 129 left) and room for the top-right of the last MB */
    int mbw = s->mbw, mbh = s->mbh;
    s->ys = mbw * 16 + 32; s->uvs = mbw * 8 + 32;
    size_t ysz = (size_t)s->ys * (mbh * 16 + 1), usz = (size_t)s->uvs * (mbh * 8 + 1);
    ba = malloc(ysz + 2 * usz);
    if (!ba) goto done;
    s->Yb = ba; s->Ub = ba + ysz; s->Vb = s->Ub + usz;
    s->Y = s->Yb + s->ys + 16; s->U = s->Ub + s->uvs + 16; s->V = s->Vb + s->uvs + 16;
    memset(s->Yb, 127, s->ys); memset(s->Ub, 127, s->uvs); memset(s->Vb, 127, s->uvs);
    for (int y = 0; y < mbh * 16; y++) s->Y[y * s->ys - 1] = 129;
    for (int y = 0; y < mbh * 8; y++) s->U[y * s->uvs - 1] = s->V[y * s->uvs - 1] = 129;
    mbs = calloc(1, sizeof *mbs);
    fi = ftype ? malloc((size_t)mbw * mbh * sizeof *fi) : NULL;
    topn = calloc(mbw, 9);                                            /* per column: 4 y, 2 u, 2 v, 1 y2 non-zero flags */
    aboveb = calloc(mbw, 4);                                          /* sub-block modes of the row above */
    if (!mbs || !topn || !aboveb || (ftype && !fi)) goto done;
    for (int my = 0; my < mbh; my++) {
        BD *tb = &s->tok[my & (s->nparts - 1)];
        uint8_t lnz[9] = { 0 }, lmode[4] = { 0 };
        for (int mx = 0; mx < mbw; mx++) {
            struct mbi *m = mbs;
            uint8_t *tn = topn + mx * 9, *tm = aboveb + mx * 4;
            /* modes */
            m->seg = 0;
            if (s->upd_map) m->seg = !bd_get(br, s->seg_p[0]) ? bd_get(br, s->seg_p[1]) : bd_get(br, s->seg_p[2]) + 2;
            m->skip = s->use_skip ? bd_get(br, s->skip_p) : 0;
            m->i4 = !bd_get(br, 145);
            if (!m->i4) {
                m->ymode = bd_get(br, 156) ? (bd_get(br, 128) ? B_TM : B_HE) : (bd_get(br, 163) ? B_VE : B_DC);
                memset(tm, m->ymode, 4); memset(lmode, m->ymode, 4);
            } else {
                for (int y = 0; y < 4; y++) {
                    int ym = lmode[y];
                    for (int x = 0; x < 4; x++) {
                        const uint8_t *pr = &kbmode[(tm[x] * 10 + ym) * 9];
                        int i = kintra4[bd_get(br, pr[0])];
                        while (i > 0) i = kintra4[2 * i + bd_get(br, pr[i])];
                        ym = -i;
                        tm[x] = ym; m->bm[y * 4 + x] = ym;
                    }
                    lmode[y] = ym;
                }
            }
            m->uvmode = !bd_get(br, 142) ? B_DC : !bd_get(br, 114) ? B_VE : bd_get(br, 183) ? B_TM : B_HE;
            /* residuals */
            int16_t co[25 * 16];
            uint8_t code[24];
            memset(co, 0, sizeof co); memset(code, 0, sizeof code);
            int nzany = 0;
            const int *q = s->qm[m->seg];
            if (!m->skip) {
                int first = 0, type = 3;
                if (!m->i4) {
                    int16_t dc[16] = { 0 };
                    int dq2[2] = { q[2], q[3] };
                    int nz = getcoeffs(s, tb, 1, tn[8] + lnz[8], dq2, 0, dc);
                    tn[8] = lnz[8] = nz > 0;
                    if (nz > 1) wht(dc, co);
                    else { int dc0 = (dc[0] + 3) >> 3; for (int i = 0; i < 16; i++) co[i * 16] = dc0; }
                    first = 1; type = 0;
                }
                int dq1[2] = { q[0], q[1] };
                for (int y = 0; y < 4; y++) {
                    int l = lnz[y];
                    for (int x = 0; x < 4; x++) {
                        int b = y * 4 + x, nz = getcoeffs(s, tb, type, l + tn[x], dq1, first, co + b * 16);
                        l = nz > first; tn[x] = l;
                        code[b] = nz > 3 ? 3 : nz > 1 ? 2 : co[b * 16] != 0;
                        nzany |= code[b];
                    }
                    lnz[y] = l;
                }
                int dqu[2] = { q[4], q[5] };
                for (int ch = 0; ch < 2; ch++)
                    for (int y = 0; y < 2; y++) {
                        int l = lnz[4 + ch * 2 + y];
                        for (int x = 0; x < 2; x++) {
                            int b = 16 + ch * 4 + y * 2 + x, nz = getcoeffs(s, tb, 2, l + tn[4 + ch * 2 + x], dqu, 0, co + b * 16);
                            l = nz > 0; tn[4 + ch * 2 + x] = l;
                            code[b] = nz > 3 ? 3 : nz > 1 ? 2 : co[b * 16] != 0;
                            nzany |= code[b];
                        }
                        lnz[4 + ch * 2 + y] = l;
                    }
            } else {
                memset(tn, 0, 8); memset(lnz, 0, 8);
                if (!m->i4) tn[8] = lnz[8] = 0;
            }
            if (ftype) { fi[my * mbw + mx] = s->fs[m->seg][m->i4]; fi[my * mbw + mx].inner |= nzany != 0; }
            /* reconstruction */
            int ys = s->ys, uvs = s->uvs;
            uint8_t *yd = s->Y + my * 16 * ys + mx * 16;
            if (!m->i4) {
                predn(yd, ys, 16, m->ymode, my > 0, mx > 0);
                for (int b = 0; b < 16; b++) if (code[b]) idct_add(co + b * 16, yd + (b >> 2) * 4 * ys + (b & 3) * 4, ys, code[b] == 1);
            } else {
                uint8_t tr[4];
                if (my > 0 && mx == mbw - 1) memset(tr, yd[-ys + 15], 4); else memcpy(tr, yd - ys + 16, 4);
                for (int b = 0; b < 16; b++) {
                    int bx = b & 3, by = b >> 2;
                    uint8_t *dst = yd + by * 4 * ys + bx * 4, A[8], Lf[4];
                    memcpy(A, dst - ys, 4);
                    if (bx < 3) memcpy(A + 4, dst - ys + 4, 4); else memcpy(A + 4, tr, 4);
                    for (int i = 0; i < 4; i++) Lf[i] = dst[i * ys - 1];
                    pred4(dst, ys, m->bm[b], A, Lf, dst[-ys - 1]);
                    if (code[b]) idct_add(co + b * 16, dst, ys, code[b] == 1);
                }
            }
            for (int ch = 0; ch < 2; ch++) {
                uint8_t *pd = (ch ? s->V : s->U) + my * 8 * uvs + mx * 8;
                predn(pd, uvs, 8, m->uvmode, my > 0, mx > 0);
                for (int b = 0; b < 4; b++) {
                    int k = 16 + ch * 4 + b;
                    if (code[k]) idct_add(co + k * 16, pd + (b >> 1) * 4 * uvs + (b & 1) * 4, uvs, code[k] == 1);
                }
            }
        }
    }
    if (ftype) filter_frame(s, fi);
    /* RGB with fancy upsampling */
    out = malloc((size_t)w * h * 4 + 4);
    if (out) {
        const uint8_t *Y = s->Y, *U = s->U, *V = s->V;
        int ys = s->ys, uvs = s->uvs;
        up_pair(Y, NULL, U, V, U, V, out, NULL, w);
        int y;
        for (y = 1; y + 1 < h; y += 2) {
            int cr = y >> 1;
            up_pair(Y + y * ys, Y + (y + 1) * ys, U + cr * uvs, V + cr * uvs, U + (cr + 1) * uvs, V + (cr + 1) * uvs,
                    out + (size_t)y * w, out + (size_t)(y + 1) * w, w);
        }
        if (h > 1 && !(h & 1)) {                                       /* the last row has no chroma row below it */
            int cr = (h - 1) >> 1;
            up_pair(Y + (h - 1) * ys, NULL, U + cr * uvs, V + cr * uvs, U + cr * uvs, V + cr * uvs, out + (size_t)(h - 1) * w, NULL, w);
        }
    }
    *ow = w; *oh = h;
done:
    free(ba); free(mbs); free(fi); free(topn); free(aboveb); free(s);
    return out;
}

/* ======================= container ======================= */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int alpha_apply(uint32_t *px, int w, int h, const uint8_t *a, size_t an) {
    if (an < 1) return -1;
    int comp = a[0] & 3, filt = (a[0] >> 2) & 3;
    size_t np = (size_t)w * h;
    uint8_t *pl = malloc(np + 1);
    if (!pl) return -1;
    if (comp == 0) {
        if (an - 1 < np) { free(pl); return -1; }
        memcpy(pl, a + 1, np);
    } else if (comp == 1) {
        int ww = w, hh = h;
        uint32_t *g = vl_decode(a + 1, an - 1, &ww, &hh, 1);
        if (!g) { free(pl); return -1; }
        for (size_t i = 0; i < np; i++) pl[i] = g[i] >> 8;
        free(g);
    } else { free(pl); return -1; }
    if (filt) {
        for (int y = 0; y < h; y++) {
            uint8_t *r = pl + (size_t)y * w, *u = r - w;
            for (int x = 0; x < w; x++) {
                int p;
                if (y == 0) p = x ? r[x - 1] : 0;
                else if (x == 0) p = u[0];
                else p = filt == 1 ? r[x - 1] : filt == 2 ? u[x] : clip8(r[x - 1] + u[x] - u[x - 1]);
                r[x] += p;
            }
        }
    }
    for (size_t i = 0; i < np; i++) px[i] = (px[i] & 0x00FFFFFFu) | (uint32_t)pl[i] << 24;
    free(pl);
    return 0;
}

/* first picture among the chunks in d[0..n): ALPH + VP8, or VP8L */
static uint32_t *frame_decode(const uint8_t *d, size_t n, int *w, int *h) {
    const uint8_t *alph = NULL;
    size_t alen = 0;
    for (size_t p = 0; p + 8 <= n; ) {
        size_t sz = le32(d + p + 4), pl = p + 8;
        if (sz > n - pl) sz = n - pl;
        if (!memcmp(d + p, "ALPH", 4)) { alph = d + pl; alen = sz; }
        else if (!memcmp(d + p, "VP8 ", 4)) {
            uint32_t *px = vp8_decode(d + pl, sz, w, h);
            if (px && alph) alpha_apply(px, *w, *h, alph, alen);
            return px;
        } else if (!memcmp(d + p, "VP8L", 4)) {
            uint32_t *px = vl_decode(d + pl, sz, w, h, 0);
            if (px && !(sz > 4 && (d[pl + 4] & 0x10))) for (size_t i = 0; i < (size_t)*w * *h; i++) px[i] |= 0xFF000000u;
            return px;
        }
        p = pl + sz + (sz & 1);
    }
    return NULL;
}

int webp_decode(const uint8_t *d, size_t n, struct image *im) {
    im->w = im->h = 0; im->px = NULL;
    if (n < 20 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4)) return -1;
    size_t rs = le32(d + 4) + 8;
    if (rs < n) n = rs;
    int cw = 0, ch = 0, w, h;
    for (size_t p = 12; p + 8 <= n; ) {
        size_t sz = le32(d + p + 4), pl = p + 8;
        if (sz > n - pl) sz = n - pl;
        if (!memcmp(d + p, "VP8X", 4) && sz >= 10) { cw = (d[pl + 4] | d[pl + 5] << 8 | d[pl + 6] << 16) + 1; ch = (d[pl + 7] | d[pl + 8] << 8 | d[pl + 9] << 16) + 1; }
        if (!memcmp(d + p, "ANMF", 4) && sz > 16) {                      /* animation: its first frame, on the canvas */
            uint32_t *f = frame_decode(d + pl + 16, sz - 16, &w, &h);
            if (!f) return -1;
            int fx = 2 * (d[pl] | d[pl + 1] << 8 | d[pl + 2] << 16), fy = 2 * (d[pl + 3] | d[pl + 4] << 8 | d[pl + 5] << 16);
            if (cw < 1 || ch < 1 || (unsigned)cw * ch > WEBP_MAXPIX) { free(f); return -1; }
            uint32_t *c = calloc((size_t)cw * ch, 4);
            if (!c) { free(f); return -1; }
            for (int y = 0; y < h && fy + y < ch; y++)
                for (int x = 0; x < w && fx + x < cw; x++) c[(size_t)(fy + y) * cw + fx + x] = f[(size_t)y * w + x];
            free(f);
            im->w = cw; im->h = ch; im->px = c;
            return 0;
        }
        if (!memcmp(d + p, "VP8 ", 4) || !memcmp(d + p, "VP8L", 4) || !memcmp(d + p, "ALPH", 4)) {
            uint32_t *f = frame_decode(d + p, n - p, &w, &h);
            if (!f) return -1;
            im->w = w; im->h = h; im->px = f;
            return 0;
        }
        p = pl + sz + (sz & 1);
    }
    return -1;
}
