/* fb.c - linear framebuffer + the boot/panic text console */
#include "console.h"
#include "arch.h"
#include "theme.h"
#include "design.h"
#include "icons.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

struct canvas fb;
static bool con_on;
static int con_x, con_y;
static const struct font *con_font = &font_mono;
#define CON_FG RGB(0xD7, 0xD9, 0xDC)
#define CON_BG RGB(0x11, 0x12, 0x14)

/* boot splash: the wordmark, a progress bar and the last kprintf line under it, on the Graphite tokens (the theme
 * isn't loaded yet, and the panic screen must not depend on it either) */
static bool splash_on;
static int splash_pct;
static char status[96];
static int st_n;
static bool st_bad;
#define G (theme_get(0))
#define BAR_W 240

static bool irqs_on(void) { uint32_t f; __asm__ volatile("pushf; pop %0" : "=r"(f)); return f & 0x200; }

static void splash_draw_status(void) {
    int y = fb.h / 2 + 36;
    gfx_fill(&fb, 0, y, fb.w, T_CAPTION->height, G->base);
    status[st_n] = 0;
    gfx_text_center(&fb, T_CAPTION, 0, y, fb.w, status, st_bad ? G->danger : G->text3);
}

void splash_progress(int pct) {
    if (!splash_on) return;
    int x = (fb.w - BAR_W) / 2, y = fb.h / 2 + 16;
    for (; splash_pct <= pct; splash_pct++) {          /* sweep so jumps animate a little */
        gfx_round_fill(&fb, x, y, BAR_W, 4, R_1, G->line);
        gfx_round_fill(&fb, x, y, imax(4, BAR_W * splash_pct / 100), 4, R_1, G->accent);
        uint64_t until = ticks_ms + 3;
        while (ticks_ms < until && irqs_on()) hlt();
    }
}

static void splash_init(void) {
    gfx_fill(&fb, 0, 0, fb.w, fb.h, G->base);
    gfx_text_center(&fb, T_DISPLAY, 0, fb.h / 2 - 24 - T_DISPLAY->height, fb.w, "VibeOS", G->text);
    st_bad = false;
    splash_on = true;
    splash_progress(0);
}

void fb_init(void) {
    canvas_init(&fb, (uint32_t *)boot_info->fb_addr, boot_info->fb_width,
                boot_info->fb_height, boot_info->fb_pitch / 4);
    con_x = 16; con_y = 12;
    con_on = true;
    splash_init();
}

void fb_set_mode(uint32_t *px, int w, int h, int stride) {
    canvas_init(&fb, px, w, h, stride);
    con_x = 16; con_y = 12;
    if (splash_on) { int p = splash_pct - 1; splash_pct = 0; splash_init(); splash_progress(p); splash_draw_status(); }
}

void bootcon_enable(bool on) { con_on = on; if (!on) splash_on = false; }

static void con_newline(void) {
    int lh = con_font->height;
    con_x = 16;
    con_y += lh;
    if (con_y + lh > fb.h - 12) {                     /* scroll */
        int rows = fb.h - 12 - lh;
        memmove(fb.px, fb.px + lh * fb.stride, (size_t)rows * fb.stride * 4);
        gfx_fill(&fb, 0, rows, fb.w, fb.h - rows, CON_BG);
        con_y -= lh;
    }
}

void bootcon_write(const char *s) {
    if (!con_on || !fb.px) return;
    if (splash_on) {
        for (; *s; s++) {
            if (*s == '\n') { splash_draw_status(); st_n = 0; st_bad = false; }
            else if (*s == '\x1b') {                   /* colour codes: only red matters here */
                if (s[1] == '[' && s[2] == '3' && s[3] == '1') st_bad = true;
                while (*s && *s != 'm') s++;
                if (!*s) break;
            } else if (st_n < (int)sizeof status - 1) status[st_n++] = *s;
        }
        return;
    }
    static color_t col = CON_FG;
    for (; *s; s++) {
        if (*s == '\n') { con_newline(); col = CON_FG; continue; }
        if (*s == '\x1b' && s[1] == '[') {            /* tiny ESC[3Xm colour support */
            int v = 0; s += 2;
            while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
            static const color_t pal[8] = { 0xFF505870, 0xFFFF6B7A, 0xFF7EE787, 0xFFFFD166,
                                            0xFF7AB8FF, 0xFFD69CFF, 0xFF6FE3E3, 0xFFF0F0F0 };
            col = v >= 30 && v <= 37 ? pal[v - 30] : CON_FG;
            continue;
        }
        if (con_x + con_font->g[0].adv > fb.w - 16) con_newline();
        con_x += gfx_char(&fb, con_font, con_x, con_y, *s, col);
    }
}

static void stack_trace(char *buf, size_t n, uint32_t ebp) {
    size_t pos = 0;
    for (int i = 0; i < 8 && ebp >= 0x100000 && ebp < 0x40000000 && !(ebp & 3); i++) {
        uint32_t ret = ((uint32_t *)ebp)[1];
        pos += snprintf(buf + pos, n - pos, "%08x ", ret);
        if (pos >= n) break;
        ebp = ((uint32_t *)ebp)[0];
    }
}

/* the panic card: what happened, the registers and a trace, on the Graphite tokens over a scrim */
NORETURN void bootcon_panic(const char *msg, struct regs *r) {
    char line[160];
    kprintf("\n\x1b[31mKERNEL PANIC: %s\x1b[0m\n", msg);
    con_on = false;                                  /* the rest goes to serial only */
    if (fb.px) {
        canvas_noclip(&fb);
        const struct font *m = T_MONO;
        int w = imin(720, fb.w - 2 * SP_5), pad = SP_6;
        int h = pad + 24 + SP_3 + T_BODY->height + SP_5 + m->height + (r ? SP_5 + 5 * m->height : 0) + SP_6 + T_BODY->height + pad;
        int x = (fb.w - w) / 2, y = imax(SP_5, (fb.h - h) / 2), tx = x + pad;
        gfx_fill(&fb, 0, 0, fb.w, fb.h, RGBA(0, 0, 0, 166));
        gfx_elev(&fb, x, y, w, h, R_3, E4, G->elev[E4]);
        gfx_round_fill(&fb, x, y, w, h, R_3, G->overlay);
        gfx_round_rect(&fb, x, y, w, h, R_3, G->line);
        const struct icon *ic = icon_get(ICON_ERROR, ICON_MD);
        int ly = y + pad;
        if (ic) gfx_mask(&fb, tx, ly + 2, ic->a8, ic->w, ic->h, ic->w, G->danger);
        gfx_text(&fb, T_HEADING, tx + ICON_MD + SP_3, ly + 2, "VibeOS stopped: kernel panic", G->text);
        ly += 24 + SP_3;
        gfx_text(&fb, T_BODY, tx, ly, "Something went wrong inside the kernel, so it halted to keep your files safe.", G->text2);
        ly += T_BODY->height + SP_5;
        snprintf(line, sizeof line, "%s", msg);
        gfx_text_ell(&fb, m, tx, ly, w - 2 * pad, line, G->danger);
        ly += m->height;
        if (r) {
            ly += SP_5;
            snprintf(line, sizeof line, "EIP %08x  CS %04x  EFLAGS %08x  CR2 %08x", r->eip, r->cs, r->eflags, read_cr2());
            gfx_text(&fb, m, tx, ly, line, G->text); ly += m->height;
            snprintf(line, sizeof line, "EAX %08x  EBX %08x  ECX %08x  EDX %08x", r->eax, r->ebx, r->ecx, r->edx);
            gfx_text(&fb, m, tx, ly, line, G->text2); ly += m->height;
            snprintf(line, sizeof line, "ESI %08x  EDI %08x  EBP %08x  ESP %08x", r->esi, r->edi, r->ebp, r->esp0);
            gfx_text(&fb, m, tx, ly, line, G->text2); ly += m->height;
            snprintf(line, sizeof line, "DS %04x  ES %04x  FS %04x  GS %04x  CR3 %08x", r->ds, r->es, r->fs, r->gs, read_cr3());
            gfx_text(&fb, m, tx, ly, line, G->text2); ly += m->height;
            char tr[120] = "";
            stack_trace(tr, sizeof tr, r->ebp);
            snprintf(line, sizeof line, "Trace  %s", tr);
            gfx_text_ell(&fb, m, tx, ly, w - 2 * pad, line, G->text2); ly += m->height;
            kprintf("EIP=%08x CS=%x EFLAGS=%08x CR2=%08x\nEAX=%08x EBX=%08x ECX=%08x EDX=%08x\n"
                    "ESI=%08x EDI=%08x EBP=%08x\n%s\n",
                    r->eip, r->cs, r->eflags, read_cr2(), r->eax, r->ebx, r->ecx, r->edx,
                    r->esi, r->edi, r->ebp, line);
        }
        gfx_text(&fb, T_BODY, tx, y + h - pad - T_BODY->height, "Restart the machine to continue. The details are also in the serial log.", G->text2);
    }
    for (;;) { cli(); hlt(); }
}
