/* fb.c - linear framebuffer + the boot/panic text console */
#include "console.h"
#include "arch.h"

struct canvas fb;
static bool con_on;
static int con_x, con_y;
static const struct font *con_font = &font_mono;
#define CON_FG RGB(0xC8, 0xCF, 0xE0)
#define CON_BG RGB(0x0E, 0x10, 0x18)

/* boot splash: logo + progress bar; kprintf lines become the status text under it */
static bool splash_on;
static int splash_pct;
static char status[96];
static int st_n;
static color_t st_col;
#define BAR_W 300

static bool irqs_on(void) { uint32_t f; __asm__ volatile("pushf; pop %0" : "=r"(f)); return f & 0x200; }

static void splash_draw_status(void) {
    int y = fb.h / 2 + 96;
    gfx_fill(&fb, 0, y, fb.w, font_ui.height + 4, RGB(0x12, 0x0F, 0x22));
    status[st_n] = 0;
    gfx_text_center(&fb, &font_ui, 0, y, fb.w, status, st_col);
}

void splash_progress(int pct) {
    if (!splash_on) return;
    int x = (fb.w - BAR_W) / 2, y = fb.h / 2 + 74;
    for (; splash_pct <= pct; splash_pct++) {          /* sweep so jumps animate a little */
        int fill = BAR_W * splash_pct / 100;
        gfx_round_fill(&fb, x, y, BAR_W, 6, 3, RGB(0x2A, 0x26, 0x40));
        for (int i = 0; i < fill; i++)
            gfx_fill(&fb, x + i, y, 1, 6, mix(RGB(0xB3, 0x88, 0xFF), RGB(0xFF, 0x7A, 0xC6), i * 256 / BAR_W));
        uint64_t until = ticks_ms + 3;
        while (ticks_ms < until && irqs_on()) hlt();
    }
}

static void splash_init(void) {
    gfx_fill(&fb, 0, 0, fb.w, fb.h, RGB(0x12, 0x0F, 0x22));
    int cx = fb.w / 2, cy = fb.h / 2 - 60;
    for (int i = 0; i < 44; i++) {                      /* diamond logo */
        color_t col = mix(RGB(0xB3, 0x88, 0xFF), RGB(0xFF, 0x7A, 0xC6), i * 256 / 44);
        gfx_fill(&fb, cx - i, cy - 44 + i, 2 * i + 1, 1, col);
        gfx_fill(&fb, cx - i, cy + 43 - i, 2 * i + 1, 1, mix(col, RGB(0x12, 0x0F, 0x22), 40));
    }
    gfx_text_center(&fb, &font_big, 0, cy + 58, fb.w, "VibeOS", RGB(0xED, 0xEB, 0xFF));
    st_col = RGB(0x9A, 0x96, 0xB8);
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
            if (*s == '\n') { splash_draw_status(); st_n = 0; st_col = RGB(0x9A, 0x96, 0xB8); }
            else if (*s == '\x1b') {                   /* colour codes: only red matters here */
                if (s[1] == '[' && s[2] == '3' && s[3] == '1') st_col = RGB(0xFF, 0x6B, 0x7A);
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

NORETURN void bootcon_panic(const char *msg, struct regs *r) {
    char line[160];
    kprintf("\n\x1b[31mKERNEL PANIC: %s\x1b[0m\n", msg);
    con_on = false;                                  /* the rest goes to serial only */
    if (fb.px) {
        canvas_noclip(&fb);
        int w = 760, h = r ? 300 : 120, x = (fb.w - w) / 2, y = (fb.h - h) / 2;
        gfx_fill(&fb, 0, 0, fb.w, fb.h, RGBA(10, 0, 0, 170));
        gfx_shadow(&fb, x, y, w, h, 14, 16, 90);
        gfx_round_fill(&fb, x, y, w, h, 14, RGB(0x1A, 0x0C, 0x14));
        gfx_round_rect(&fb, x, y, w, h, 14, RGB(0xFF, 0x4D, 0x6D));
        gfx_text(&fb, &font_title, x + 24, y + 18, "VibeOS has lost its vibe", RGB(0xFF, 0x6B, 0x86));
        gfx_text(&fb, &font_mono, x + 24, y + 58, msg, RGB(0xF0, 0xE0, 0xE6));
        if (r) {
            const struct font *f = &font_mono;
            int ly = y + 96;
            snprintf(line, sizeof line, "EIP %08x  CS %04x  EFLAGS %08x  CR2 %08x", r->eip, r->cs, r->eflags, read_cr2());
            gfx_text(&fb, f, x + 24, ly, line, RGB(0xFF, 0xD1, 0x66)); ly += 24;
            snprintf(line, sizeof line, "EAX %08x  EBX %08x  ECX %08x  EDX %08x", r->eax, r->ebx, r->ecx, r->edx);
            gfx_text(&fb, f, x + 24, ly, line, RGB(0xC8, 0xCF, 0xE0)); ly += 22;
            snprintf(line, sizeof line, "ESI %08x  EDI %08x  EBP %08x  ESP %08x", r->esi, r->edi, r->ebp, r->esp0);
            gfx_text(&fb, f, x + 24, ly, line, RGB(0xC8, 0xCF, 0xE0)); ly += 22;
            snprintf(line, sizeof line, "DS %04x ES %04x FS %04x GS %04x  CR3 %08x", r->ds, r->es, r->fs, r->gs, read_cr3());
            gfx_text(&fb, f, x + 24, ly, line, RGB(0xC8, 0xCF, 0xE0)); ly += 30;
            char tr[120] = "";
            stack_trace(tr, sizeof tr, r->ebp);
            snprintf(line, sizeof line, "trace: %s", tr);
            gfx_text(&fb, f, x + 24, ly, line, RGB(0x9A, 0xA3, 0xB8));
            kprintf("EIP=%08x CS=%x EFLAGS=%08x CR2=%08x\nEAX=%08x EBX=%08x ECX=%08x EDX=%08x\n"
                    "ESI=%08x EDI=%08x EBP=%08x\n%s\n",
                    r->eip, r->cs, r->eflags, read_cr2(), r->eax, r->ebx, r->ecx, r->edx,
                    r->esi, r->edi, r->ebp, line);
        }
        gfx_text(&fb, &font_ui, x + 24, y + h - 34, "The system has been halted. Restart the machine to continue.",
                 RGB(0x9A, 0x8A, 0x96));
    }
    for (;;) { cli(); hlt(); }
}
