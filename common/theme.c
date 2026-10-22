/* theme.c - the built-in themes (dark is the original look and the default), accent presets, contrast maths.
 * Freestanding; shared by the kernel, libgui and the host contrast test (tools/themetest). */
#include "theme.h"

#define H(x)     (0xFF000000u | (x))
#define HA(x, a) ((uint32_t)(a) << 24 | (x))

static const struct theme themes[] = {
    {   /* the original look */
        .name = "dark", .label = "Vibe Dark", .dark = 1,
        .bg = H(0x161522), .panel = H(0x1E1C2D), .surface = H(0x2A273D), .hover = H(0x35314C), .pressed = H(0x222033),
        .border = HA(0xFFFFFF, 26), .input = H(0x12111C),
        .text = H(0xEDEBFF), .dim = H(0x9A96B8), .faint = H(0x5E5A7C),
        .accent = H(0xB388FF), .accent_hi = H(0xC4A0FF), .accent_lo = H(0x956CE0), .on_accent = H(0x1A102E), .accent2 = H(0xFF7AC6),
        .bad = H(0xFF5F7A), .warn = H(0xFFC24B), .good = H(0x4BE08A),
        .knob = H(0xE8DFFF), .knob_on = H(0xFFFFFF), .ov = H(0xFFFFFF),
        .key = H(0x3B3062), .key_hi = H(0x4A3B78), .key_lo = H(0x3A2E5C), .key_fg = H(0xD9C4FF), .gutter = H(0x191826),
        .edge = 100, .shadow = 100,
        .frame = H(0x242236), .frame_off = H(0x1B1A26), .btn_off = H(0x454358), .cdim = H(0x8C89A6), .body = H(0xC4C1DC), .on_pill = H(0xFFFFFF),
        .bar = HA(0x0D0C16, 222), .launcher = HA(0x161422, 246), .popup = HA(0x161422, 248), .toast = HA(0x181626, 248), .switcher = HA(0x120F1E, 244),
        .term = { { H(0x2A2E42), H(0xF7768E), H(0x9ECE6A), H(0xE0AF68), H(0x7AA2F7), H(0xBB9AF7), H(0x7DCFFF), H(0xA9B1D6),
                    H(0x4E5575), H(0xFF899D), H(0xB3E07E), H(0xF5C27C), H(0x8DB0FF), H(0xC7A9FF), H(0xA4DAFF), H(0xE6EAFA) },
                  H(0xC8CEE8), H(0x11131C), H(0xF2F4FB), H(0x4A3F7A), H(0x6A5622), H(0xE0AF68), H(0x11131C), H(0x7DCFFF) },
        .tbar = H(0x161824), .tab = H(0x24283B), .tab_off = H(0x1D2031), .tab_fg = H(0xE6EAFA), .tab_dim = H(0x8D93B5), .tab_faint = H(0x6A7090),
        .tfind = H(0x202438), .tab_acc = H(0x7AA2F7),
    },
    {
        .name = "light", .label = "Vibe Light", .dark = 0,
        .bg = H(0xF3F2F9), .panel = H(0xFFFFFF), .surface = H(0xE7E5F1), .hover = H(0xDAD7EA), .pressed = H(0xCFCCE2),
        .border = HA(0x000000, 36), .input = H(0xFFFFFF),
        .text = H(0x1B1830), .dim = H(0x55526F), .faint = H(0x8F8CA9),
        .accent = H(0x6A3DD8), .accent_hi = H(0x7F57E6), .accent_lo = H(0x5429B8), .on_accent = H(0xFFFFFF), .accent2 = H(0xCC2E8C),
        .bad = H(0xC21F3F), .warn = H(0x9A5B00), .good = H(0x11803F),
        .knob = H(0x3B2F73), .knob_on = H(0x1B1830), .ov = H(0x000000),
        .key = H(0xE0D8F7), .key_hi = H(0xD3C8F2), .key_lo = H(0xC9BCEE), .key_fg = H(0x4426A8), .gutter = H(0xF0EEF8),
        .edge = 130, .shadow = 55,
        .frame = H(0xE6E3F3), .frame_off = H(0xEFEDF7), .btn_off = H(0xC4C0D8), .cdim = H(0x5A5774), .body = H(0x3F3C5A), .on_pill = H(0xFFFFFF),
        .bar = HA(0xF6F5FC, 228), .launcher = HA(0xFBFAFF, 246), .popup = HA(0xFBFAFF, 248), .toast = HA(0xFFFFFF, 248), .switcher = HA(0xF7F6FD, 244),
        .term = { { H(0x2A2E42), H(0xC2203F), H(0x2F7D1A), H(0x8A5A00), H(0x2652C4), H(0x8A3BD1), H(0x0B7285), H(0x575C78),
                    H(0x6E7390), H(0xD4294A), H(0x2E7D32), H(0x996400), H(0x2F61D6), H(0x9A48E0), H(0x0E7F96), H(0x2B2E45) },
                  H(0x23213A), H(0xFBFAFF), H(0x0E0D1C), H(0xCFC6F5), H(0xF6E6A8), H(0xF3A93C), H(0x1B1830), H(0x0A63B8) },
        .tbar = H(0xE3E1F0), .tab = H(0xFBFAFF), .tab_off = H(0xD8D5EA), .tab_fg = H(0x1B1830), .tab_dim = H(0x55526F), .tab_faint = H(0x7C7996),
        .tfind = H(0xFFFFFF), .tab_acc = H(0x6A3DD8),
    },
    {
        .name = "nord", .label = "Nord", .dark = 1,
        .bg = H(0x2E3440), .panel = H(0x343B49), .surface = H(0x3B4252), .hover = H(0x434C5E), .pressed = H(0x2A303B),
        .border = HA(0xFFFFFF, 30), .input = H(0x2A303B),
        .text = H(0xECEFF4), .dim = H(0xAEB8CB), .faint = H(0x6B7791),
        .accent = H(0x88C0D0), .accent_hi = H(0xA1D0DE), .accent_lo = H(0x6EA6B8), .on_accent = H(0x2E3440), .accent2 = H(0xB48EAD),
        .bad = H(0xF08A93), .warn = H(0xEBCB8B), .good = H(0xA3BE8C),
        .knob = H(0xD8DEE9), .knob_on = H(0xFFFFFF), .ov = H(0xFFFFFF),
        .key = H(0x445568), .key_hi = H(0x4F6277), .key_lo = H(0x3E4C5E), .key_fg = H(0xC5E4EC), .gutter = H(0x313746),
        .edge = 100, .shadow = 100,
        .frame = H(0x3B4252), .frame_off = H(0x313745), .btn_off = H(0x4C566A), .cdim = H(0xA3AEC2), .body = H(0xCBD3E1), .on_pill = H(0x2E3440),
        .bar = HA(0x242933, 226), .launcher = HA(0x2E3440, 246), .popup = HA(0x2E3440, 248), .toast = HA(0x3B4252, 248), .switcher = HA(0x2B303B, 244),
        .term = { { H(0x3B4252), H(0xF08A93), H(0xA3BE8C), H(0xEBCB8B), H(0x8BAFD6), H(0xB48EAD), H(0x88C0D0), H(0xE5E9F0),
                    H(0x6A768F), H(0xEB8A92), H(0xB5CFA0), H(0xF2D9A0), H(0x9CB9D8), H(0xC6A3BF), H(0x9FD2E0), H(0xFFFFFF) },
                  H(0xD8DEE9), H(0x2A2F3A), H(0xECEFF4), H(0x434C5E), H(0x45597A), H(0xEBCB8B), H(0x2E3440), H(0x88C0D0) },
        .tbar = H(0x252A33), .tab = H(0x2A2F3A), .tab_off = H(0x30363F), .tab_fg = H(0xECEFF4), .tab_dim = H(0xA3AEC2), .tab_faint = H(0x7C88A2),
        .tfind = H(0x3B4252), .tab_acc = H(0x88C0D0),
    },
    {
        .name = "solarized", .label = "Solarized", .dark = 1,
        .bg = H(0x002B36), .panel = H(0x03313D), .surface = H(0x073642), .hover = H(0x0F4755), .pressed = H(0x00252F),
        .border = HA(0xEEE8D5, 36), .input = H(0x00232C),
        .text = H(0xEEE8D5), .dim = H(0x93A1A1), .faint = H(0x586E75),
        .accent = H(0x2F9BE0), .accent_hi = H(0x4FB0EE), .accent_lo = H(0x2480C0), .on_accent = H(0x002B36), .accent2 = H(0xD33682),
        .bad = H(0xF5706C), .warn = H(0xD9A21B), .good = H(0x9DB300),
        .knob = H(0x93A1A1), .knob_on = H(0xEEE8D5), .ov = H(0xEEE8D5),
        .key = H(0x0F4F5E), .key_hi = H(0x175A6A), .key_lo = H(0x0B4552), .key_fg = H(0x8FD6EE), .gutter = H(0x00303B),
        .edge = 100, .shadow = 100,
        .frame = H(0x073642), .frame_off = H(0x04303B), .btn_off = H(0x586E75), .cdim = H(0xAAB7B7), .body = H(0xCFC9B5), .on_pill = H(0x002B36),
        .bar = HA(0x00222B, 226), .launcher = HA(0x00303B, 246), .popup = HA(0x03313D, 248), .toast = HA(0x073642, 248), .switcher = HA(0x00252E, 244),
        .term = { { H(0x073642), H(0xF0605C), H(0x9BB100), H(0xD0A010), H(0x3F9FE5), H(0xEC62A4), H(0x36B8AD), H(0xEEE8D5),
                    H(0x6C858D), H(0xE8683A), H(0xA6C400), H(0xE3B21E), H(0x5DB2F0), H(0x8E93E0), H(0x4ECBC0), H(0xFDF6E3) },
                  H(0xB0BDBD), H(0x002B36), H(0xEEE8D5), H(0x0B4452), H(0x4A4A12), H(0xB58900), H(0x002B36), H(0x3DBDB2) },
        .tbar = H(0x00222B), .tab = H(0x002B36), .tab_off = H(0x03303A), .tab_fg = H(0xEEE8D5), .tab_dim = H(0x93A1A1), .tab_faint = H(0x6A8087),
        .tfind = H(0x073642), .tab_acc = H(0x2F9BE0),
    },
    {
        .name = "contrast", .label = "High Contrast", .dark = 1,
        .bg = H(0x000000), .panel = H(0x0B0B0B), .surface = H(0x1C1C1C), .hover = H(0x333333), .pressed = H(0x101010),
        .border = HA(0xFFFFFF, 150), .input = H(0x000000),
        .text = H(0xFFFFFF), .dim = H(0xDADADA), .faint = H(0x9A9A9A),
        .accent = H(0xFFD60A), .accent_hi = H(0xFFE45C), .accent_lo = H(0xE0B800), .on_accent = H(0x000000), .accent2 = H(0x4DD8FF),
        .bad = H(0xFF7B7B), .warn = H(0xFFD60A), .good = H(0x5BFF8F),
        .knob = H(0xFFFFFF), .knob_on = H(0xFFD60A), .ov = H(0xFFFFFF),
        .key = H(0x4A4000), .key_hi = H(0x665A00), .key_lo = H(0x332C00), .key_fg = H(0xFFE45C), .gutter = H(0x0B0B0B),
        .edge = 400, .shadow = 100,
        .frame = H(0x1A1A1A), .frame_off = H(0x0A0A0A), .btn_off = H(0x6A6A6A), .cdim = H(0xD0D0D0), .body = H(0xF0F0F0), .on_pill = H(0x000000),
        .bar = HA(0x000000, 240), .launcher = HA(0x000000, 250), .popup = HA(0x0A0A0A, 250), .toast = HA(0x0A0A0A, 250), .switcher = HA(0x000000, 250),
        .term = { { H(0x000000), H(0xFF6B6B), H(0x5BFF8F), H(0xFFD60A), H(0x6EA8FF), H(0xE08CFF), H(0x4DD8FF), H(0xE0E0E0),
                    H(0x8A8A8A), H(0xFF9A9A), H(0x8CFFB0), H(0xFFE55C), H(0x9CC4FF), H(0xEEB0FF), H(0x8CE8FF), H(0xFFFFFF) },
                  H(0xFFFFFF), H(0x000000), H(0xFFFFFF), H(0x1A4DB3), H(0x705800), H(0xFFD60A), H(0x000000), H(0x4DD8FF) },
        .tbar = H(0x111111), .tab = H(0x000000), .tab_off = H(0x262626), .tab_fg = H(0xFFFFFF), .tab_dim = H(0xD0D0D0), .tab_faint = H(0x909090),
        .tfind = H(0x1A1A1A), .tab_acc = H(0xFFD60A),
    },
};
#define NTHEMES (int)(sizeof themes / sizeof themes[0])

const struct theme_accent theme_accents[THEME_ACCENTS] = {
    { "violet", H(0x8B5CF6) }, { "blue", H(0x3B82F6) }, { "teal", H(0x14A39A) }, { "green", H(0x22A55B) }, { "yellow", H(0xF5B301) },
    { "orange", H(0xF97316) }, { "red", H(0xEF4444) }, { "pink", H(0xEC4899) }, { "graphite", H(0x8A8FA3) },
};

int theme_count(void) { return NTHEMES; }
const struct theme *theme_get(int i) { return &themes[(i % NTHEMES + NTHEMES) % NTHEMES]; }

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
int theme_find(const char *name) {
    for (int i = 0; i < NTHEMES; i++) if (streq(themes[i].name, name)) return i;
    return -1;
}
int theme_accent_find(const char *name) {
    for (int i = 0; i < THEME_ACCENTS; i++) if (streq(theme_accents[i].name, name)) return i;
    return -1;
}

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

/* the first of the candidates that reads at 4.5:1 on bg (black always does once white can't) */
static color_t readable(color_t bg, color_t first) {
    color_t c[3] = { first, H(0xFFFFFF), H(0x000000) };
    for (int i = 0; i < 3; i++) if (theme_contrast(c[i], bg) >= 450) return c[i];
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

void theme_set_accent(struct theme *t, color_t rgb) {
    if (!rgb) return;
    color_t surf[5] = { t->bg, t->panel, t->frame, t->popup, t->toast };
    rgb = reach(rgb | 0xFF000000u, surf, 5, 300, t->dark);              /* a mark on the window, so 3:1 */
    t->accent = rgb;
    t->accent_hi = mixc(rgb, H(0xFFFFFF), 56);
    t->accent_lo = mixc(rgb, H(0x000000), 46);
    t->on_accent = readable(rgb, H(0x1A102E));
    t->key = mixc(t->surface, rgb, 40);
    t->key_hi = mixc(t->key, rgb, 24);
    t->key_lo = mixc(t->key, H(0x000000), 20);
    color_t kb[3] = { t->key, t->key_hi, t->key_lo };
    t->key_fg = reach(mixc(rgb, H(t->dark ? 0xFFFFFF : 0x000000), t->dark ? 128 : 150), kb, 3, 450, t->dark);
    t->on_pill = readable(mixc(rgb, t->accent2, 128), H(0xFFFFFF));
}
