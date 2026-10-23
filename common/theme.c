/* theme.c - the built-in themes (graphite is the default), accent presets, contrast maths.
 * Each table entry sets the Graphite tokens and the terminal palette.
 * Freestanding; shared by the kernel, libgui and the host contrast test (tools/themetest). */
#include "theme.h"

#define H(x)     (0xFF000000u | (x))
#define HA(x, a) ((uint32_t)(a) << 24 | (x))

static struct theme themes[] = {
    {
        .name = "graphite", .label = "Dark", .dark = 1,
        .base = H(0x141517), .window = H(0x1B1C1F), .raised = H(0x232427), .overlay = H(0x2C2D31),
        .line = H(0x34363B), .line_strong = H(0x45484E), .control = H(0x74777E),
        .text = H(0xE6E7E9), .text2 = H(0xA4A7AD), .text3 = H(0x8A8D94), .disabled = HA(0xE6E7E9, 97),
        .accent = H(0xE8A33D), .accent_hover = H(0xF0B358), .accent_press = H(0xD18F2B), .on_accent = H(0x1A1300),
        .accent_text = H(0xE8A33D), .accent_line = H(0xE8A33D),
        .selection = H(0x403424), .hover_ov = HA(0xE6E7E9, 15), .press_ov = HA(0xE6E7E9, 31), .scrim = HA(0x000000, 102),
        .danger = H(0xF2706A), .on_danger = H(0x1A0605), .warning = H(0xD8B23E), .success = H(0x5BBF7A), .info = H(0x6AA3F0),
        .thumb = H(0xE6E7E9), .hilite = HA(0xFFFFFF, 15), .ring = HA(0x000000, 128),
        .elev = { 0, 77, 92, 102, 128 },
        .term = { { H(0x2C2E33), H(0xE5736C), H(0x8FBF9A), H(0xD9B45E), H(0x8FB0D9), H(0xC29BC9), H(0x7FBFBF), H(0xC5C8CD),
                    H(0x7C8088), H(0xF08C86), H(0xA8D4B1), H(0xE8C878), H(0xA9C4E6), H(0xD4B3DA), H(0x9AD2D1), H(0xEDEEF0) },
                  H(0xD7D9DC), H(0x111214), H(0xF0F1F2), H(0x403424), H(0x4A3B1E), H(0xE8A33D), H(0x1A1300), H(0x8FB0D9) },
    },
    {
        .name = "graphite-light", .label = "Light", .dark = 0,
        .base = H(0xE9EAEC), .window = H(0xF4F5F6), .raised = H(0xFFFFFF), .overlay = H(0xFFFFFF),
        .line = H(0xD6D8DB), .line_strong = H(0xB9BCC1), .control = H(0x82858C),
        .text = H(0x1B1C1F), .text2 = H(0x55585E), .text3 = H(0x66696F), .disabled = HA(0x1B1C1F, 97),
        .accent = H(0xE8A33D), .accent_hover = H(0xF0B358), .accent_press = H(0xD18F2B), .on_accent = H(0x1A1300),
        .accent_text = H(0x965509), .accent_line = H(0xAC792D),
        .selection = H(0xF1E3CD), .hover_ov = HA(0x1B1C1F, 15), .press_ov = HA(0x1B1C1F, 31), .scrim = HA(0x000000, 82),
        .danger = H(0xB42C26), .on_danger = H(0xFFFFFF), .warning = H(0x7F6200), .success = H(0x1B6E3B), .info = H(0x2560C2),
        .thumb = H(0xFFFFFF), .hilite = 0, .ring = HA(0x000000, 31),
        .elev = { 0, 36, 41, 46, 61 },
        .term = { { H(0x1B1C1F), H(0xB42C26), H(0x1B6E3B), H(0x7F6200), H(0x2560C2), H(0x8B3A8F), H(0x0E6E70), H(0x55585E),
                    H(0x66696F), H(0xC2362F), H(0x237A45), H(0x8A6A00), H(0x2F6BD0), H(0x9A479E), H(0x13797B), H(0x2B2D31) },
                  H(0x1B1C1F), H(0xFBFBFC), H(0x0E0F11), H(0xF1E3CD), H(0xF6E2AE), H(0xE8A33D), H(0x1A1300), H(0x2560C2) },
    },
    {   /* high contrast: the same structure, black surfaces, white text and outlines */
        .name = "contrast", .label = "High contrast", .dark = 1,
        .base = H(0x000000), .window = H(0x0A0A0B), .raised = H(0x18181A), .overlay = H(0x1E1E21),
        .line = H(0x8C8C90), .line_strong = H(0xD0D0D4), .control = H(0xFFFFFF),
        .text = H(0xFFFFFF), .text2 = H(0xE4E4E6), .text3 = H(0xC8C8CC), .disabled = HA(0xFFFFFF, 115),
        .accent = H(0xFFC24D), .accent_hover = H(0xFFD27A), .accent_press = H(0xE6A82E), .on_accent = H(0x000000),
        .accent_text = H(0xFFC24D), .accent_line = H(0xFFC24D),
        .selection = H(0x553C00), .hover_ov = HA(0xFFFFFF, 31), .press_ov = HA(0xFFFFFF, 56), .scrim = HA(0x000000, 166),
        .danger = H(0xFF8A80), .on_danger = H(0x000000), .warning = H(0xFFD54F), .success = H(0x7BE495), .info = H(0x8AB8FF),
        .thumb = H(0x000000), .hilite = 0, .ring = H(0x8C8C90),
        .elev = { 0, 128, 140, 150, 170 },
        .term = { { H(0x000000), H(0xFF8A80), H(0x7BE495), H(0xFFD54F), H(0x8AB8FF), H(0xF0A6FF), H(0x6FE3F0), H(0xE4E4E6),
                    H(0x9A9A9E), H(0xFFB0A8), H(0xA6F0B8), H(0xFFE38A), H(0xB5D2FF), H(0xF7C8FF), H(0xA8F0F7), H(0xFFFFFF) },
                  H(0xFFFFFF), H(0x000000), H(0xFFFFFF), H(0x553C00), H(0x6B5200), H(0xFFC24D), H(0x000000), H(0x8AB8FF) },
    },
};
#define NTHEMES (int)(sizeof themes / sizeof themes[0])

const struct theme_accent theme_accents[THEME_ACCENTS] = {
    { "amber", H(0xE8A33D) }, { "blue", H(0x4A8FE7) }, { "teal", H(0x2AA79B) }, { "green", H(0x4CAF6A) }, { "orange", H(0xF0813A) },
    { "red", H(0xE5534B) }, { "pink", H(0xE0679E) }, { "violet", H(0x9A7AE0) }, { "graphite", H(0x9A9DA3) },
};

/* ---- contrast ---- */
/* sRGB -> linear light, 0..65535 */
static const uint16_t lin16[256] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313,
    340, 367, 396, 427, 458, 491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898,
    947, 997, 1048, 1101, 1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
    1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258,
    3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124,
    5257, 5392, 5530, 5669, 5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
    7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

static uint64_t lum(color_t c) {                      /* relative luminance * 10000 * 65535 */
    return 2126ull * lin16[c >> 16 & 255] + 7152ull * lin16[c >> 8 & 255] + 722ull * lin16[c & 255];
}

int theme_contrast(color_t a, color_t b) {
    uint64_t la = lum(a), lb = lum(b), k = 10000ull * 65535 / 20;    /* the 0.05 flare term */
    if (la < lb) { uint64_t t = la; la = lb; lb = t; }
    return (int)((la + k) * 100 / (lb + k));
}

static color_t mixc(color_t a, color_t b, int t) {   /* t: 0..256 towards b, opaque */
    uint32_t rb = ((b & 0xFF00FF) * t + (a & 0xFF00FF) * (256 - t)) >> 8;
    uint32_t g = ((b & 0x00FF00) * t + (a & 0x00FF00) * (256 - t)) >> 8;
    return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}

/* the first of the candidates that reads at 4.5:1 on every bg (black always does once white can't) */
static color_t readable(const color_t *bg, int n, color_t first) {
    color_t c[3] = { first, H(0xFFFFFF), H(0x000000) };
    for (int i = 0; i < 3; i++) {
        int ok = 1;
        for (int j = 0; j < n; j++) if (theme_contrast(c[i], bg[j]) < 450) ok = 0;
        if (ok) return c[i];
    }
    return c[2];
}

/* nudge c towards white (dark themes) or black until it reads at least min/100:1 on every background */
static color_t reach(color_t c, const color_t *bg, int n, int min, int towards_white) {
    for (int step = 0; step < 12; step++) {
        int ok = 1;
        for (int i = 0; i < n; i++) if (theme_contrast(c, bg[i] | 0xFF000000u) < min) ok = 0;
        if (ok) break;
        c = mixc(c, towards_white ? H(0xFFFFFF) : H(0x000000), 30);
    }
    return c;
}

int theme_count(void) { return NTHEMES; }
const struct theme *theme_get(int i) {
    return &themes[(i % NTHEMES + NTHEMES) % NTHEMES];
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
int theme_find(const char *name) {
    if (streq(name, "dark")) name = "graphite";         /* the old ids, as saved in /etc/theme */
    else if (streq(name, "light")) name = "graphite-light";
    for (int i = 0; i < NTHEMES; i++) if (streq(themes[i].name, name)) return i;
    return -1;
}
int theme_accent_find(const char *name) {
    for (int i = 0; i < THEME_ACCENTS; i++) if (streq(theme_accents[i].name, name)) return i;
    return -1;
}

/* rgb moved up to k/256 towards to, but never so far that ink stops reading on it */
static color_t shade(color_t rgb, color_t to, int k, color_t ink) {
    color_t c;
    while (theme_contrast(ink, c = mixc(rgb, to, k)) < 450 && k > 0) k -= 6;
    return k > 0 ? c : rgb;
}

void theme_set_accent(struct theme *t, color_t rgb) {
    if (!rgb) return;
    color_t surf[4] = { t->base, t->window, t->raised, t->overlay };
    rgb |= 0xFF000000u;
    if (t->dark) rgb = reach(rgb, surf, 4, 300, 1);                     /* a mark on any dark surface, so 3:1; light fills get an accent_line rim */
    t->accent = rgb;
    t->on_accent = readable(&rgb, 1, H(0x1A1300));
    t->accent_hover = shade(rgb, H(0xFFFFFF), 40, t->on_accent);
    t->accent_press = shade(rgb, H(0x000000), 30, t->on_accent);
    t->accent_text = reach(rgb, surf, 4, 450, t->dark);
    int k = t->dark ? 46 : 51;                                           /* selection: accent ~18-20% over the window */
    do t->selection = mixc(t->window, rgb, k);
    while ((theme_contrast(t->text2, t->selection) < 450 || theme_contrast(t->text, t->selection) < 450) && (k -= 6) > 10);
    color_t marks[4] = { t->base, t->window, t->raised, t->selection };
    t->accent_line = reach(rgb, marks, 4, 300, t->dark);
}
