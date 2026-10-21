/* termtest.c - host tests for common/term.c.  Build/run via run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "term.h"

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define W(t, s) term_write(t, s, (int)strlen(s))

static char rowbuf[8][256];
static int rb;
static const char *row(struct term *t, int r) {         /* row text, trailing blanks trimmed, non-ASCII as '#' */
    char *b = rowbuf[rb++ & 7];
    int n = term_cols(t), len = 0;
    for (int i = 0; i < n; i++) { uint32_t c = term_cell_char(t, i, r); b[i] = c < 127 ? (char)c : '#'; if (c != ' ') len = i + 1; }
    b[len] = 0;
    return b;
}
#define ROW(t, r, s) do { checks++; if (strcmp(row(t, r), s)) { fails++; printf("FAIL %s:%d: row %d = \"%s\", want \"%s\"\n", __FILE__, __LINE__, r, row(t, r), s); } } while (0)

static char rep[256]; static int replen;
static void on_reply(void *ctx, const char *s, int n) { memcpy(rep + replen, s, n); replen += n; rep[replen] = 0; }

static void test_wrap_scroll(void) {
    struct term *t = term_create(10, 3);
    W(t, "ABCDEFGHIJ");
    int cx, cy; term_cursor(t, &cx, &cy);
    CHECK(cx == 9 && cy == 0);                           /* deferred wrap: still on the last cell */
    W(t, "K");
    ROW(t, 0, "ABCDEFGHIJ"); ROW(t, 1, "K");
    term_cursor(t, &cx, &cy); CHECK(cx == 1 && cy == 1);
    W(t, "\r\nL\r\nM\r\nN");                             /* forces scrolling */
    ROW(t, 0, "L"); ROW(t, 1, "M"); ROW(t, 2, "N");
    CHECK(term_scrollback_lines(t) == 2);
    ROW(t, -1, "K"); ROW(t, -2, "ABCDEFGHIJ");
    /* exact-width line then newline must not double-advance */
    term_destroy(t);
    t = term_create(4, 4);
    W(t, "abcd\r\nefgh\r\n");
    ROW(t, 0, "abcd"); ROW(t, 1, "efgh"); ROW(t, 2, "");
    /* autowrap off */
    W(t, "\x1b[?7l\x1b[1;1Hxxxxxx");
    ROW(t, 0, "xxxx");
    W(t, "\x1b[?7h");
    term_destroy(t);
    t = term_create(20, 2);
    W(t, "a\tb\bc");
    ROW(t, 0, "a       c");
    term_destroy(t);
}

static void test_region_il_dl(void) {
    struct term *t = term_create(5, 6);
    W(t, "r0\r\nr1\r\nr2\r\nr3\r\nr4\r\nr5");
    W(t, "\x1b[2;4r");                                   /* region rows 2..4 (1-based) */
    int cx, cy; term_cursor(t, &cx, &cy);
    CHECK(cx == 0 && cy == 0);                           /* DECSTBM homes the cursor */
    W(t, "\x1b[4;1H\n");                                 /* LF at region bottom scrolls region only */
    ROW(t, 0, "r0"); ROW(t, 1, "r2"); ROW(t, 2, "r3"); ROW(t, 3, ""); ROW(t, 4, "r4"); ROW(t, 5, "r5");
    CHECK(term_scrollback_lines(t) == 0);
    W(t, "\x1b[2;1H\x1bM");                              /* RI at region top scrolls down */
    ROW(t, 1, ""); ROW(t, 2, "r2"); ROW(t, 3, "r3"); ROW(t, 4, "r4");
    W(t, "\x1b[1;1H\x1bM");                              /* RI above region just stays at row 0 */
    term_cursor(t, &cx, &cy); CHECK(cy == 0);
    W(t, "\x1b[r");                                      /* reset region */
    W(t, "\x1b[2;1H\x1b[2L");                            /* IL 2 */
    ROW(t, 0, "r0"); ROW(t, 1, ""); ROW(t, 2, ""); ROW(t, 3, ""); ROW(t, 4, "r2"); ROW(t, 5, "r3");
    W(t, "\x1b[2;1H\x1b[3M");                            /* DL 3 */
    ROW(t, 1, "r2"); ROW(t, 2, "r3"); ROW(t, 3, ""); ROW(t, 5, "");
    W(t, "\x1b[1S");                                     /* SU */
    ROW(t, 0, "r2"); ROW(t, 5, "");
    W(t, "\x1b[1T");                                     /* SD */
    ROW(t, 0, ""); ROW(t, 1, "r2");
    term_destroy(t);
}

static void test_erase(void) {
    struct term *t = term_create(6, 3);
    #define FILL W(t, "\x1b[H\x1b[2Jabcdef\r\nghijkl\r\nmnopqr")
    FILL; W(t, "\x1b[2;3H\x1b[J");                       /* ED 0 */
    ROW(t, 0, "abcdef"); ROW(t, 1, "gh"); ROW(t, 2, "");
    FILL; W(t, "\x1b[2;3H\x1b[1J");                      /* ED 1: through cursor */
    ROW(t, 0, ""); ROW(t, 1, "   jkl"); ROW(t, 2, "mnopqr");
    FILL; W(t, "\x1b[2J");
    ROW(t, 0, ""); ROW(t, 1, ""); ROW(t, 2, "");
    FILL; W(t, "\x1b[2;3H\x1b[K");                       /* EL 0 */
    ROW(t, 1, "gh");
    FILL; W(t, "\x1b[2;3H\x1b[1K");                      /* EL 1 */
    ROW(t, 1, "   jkl");
    FILL; W(t, "\x1b[2;3H\x1b[2K");                      /* EL 2 */
    ROW(t, 0, "abcdef"); ROW(t, 1, ""); ROW(t, 2, "mnopqr");
    FILL; W(t, "\x1b[1;2H\x1b[2P");                      /* DCH */
    ROW(t, 0, "adef");
    FILL; W(t, "\x1b[1;2H\x1b[2@");                      /* ICH */
    ROW(t, 0, "a  bcd");
    FILL; W(t, "\x1b[1;2H\x1b[3X");                      /* ECH */
    ROW(t, 0, "a   ef");
    /* erase uses the current background (BCE) */
    W(t, "\x1b[44m\x1b[2;1H\x1b[K\x1b[0m");
    CHECK(term_cell_bg(t, 5, 1) == RGB(0x7A, 0xA2, 0xF7));
    /* ED 3 drops scrollback but keeps the screen */
    W(t, "\x1b[3;1H\n\n");
    CHECK(term_scrollback_lines(t) == 2);
    W(t, "\x1b[3J");
    CHECK(term_scrollback_lines(t) == 0);
    term_destroy(t);
}

static void test_alt_screen(void) {
    struct term *t = term_create(8, 3);
    W(t, "main\x1b[2;3H");
    W(t, "\x1b[?1049h");
    CHECK(term_on_alt_screen(t));
    ROW(t, 0, "");
    W(t, "\x1b[Halt\r\n\r\n\r\n\r\n");                   /* scroll on alt: no scrollback */
    CHECK(term_scrollback_lines(t) == 0);
    W(t, "\x1b[1;1Hvim");
    ROW(t, 0, "vim");
    W(t, "\x1b[?1049l");
    CHECK(!term_on_alt_screen(t));
    ROW(t, 0, "main");
    int cx, cy; term_cursor(t, &cx, &cy);
    CHECK(cx == 2 && cy == 1);                           /* cursor restored */
    /* back again: alt is cleared on entry */
    W(t, "\x1b[?1049h");
    ROW(t, 0, "");
    W(t, "\x1b[?1049l");
    /* scrolling view is ignored on alt */
    W(t, "\x1b[?47h"); term_scroll_view(t, 5); CHECK(term_view_offset(t) == 0);
    W(t, "\x1b[?47l");
    term_destroy(t);
}

static void test_sgr(void) {
    struct term *t = term_create(40, 2);
    W(t, "\x1b[31mR\x1b[0mn");
    CHECK(term_cell_fg(t, 0, 0) == RGB(0xF7, 0x76, 0x8E));
    CHECK(term_cell_fg(t, 1, 0) == TERM_DEFAULT_FG);
    CHECK(term_cell_bg(t, 0, 0) == TERM_DEFAULT_BG);
    W(t, "\x1b[1;4;7mX\x1b[22mY\x1b[24mZ\x1b[27mW\x1b[0m");
    CHECK(term_cell_attr(t, 2, 0) == (TERM_A_BOLD | TERM_A_UNDER | TERM_A_INVERSE));
    CHECK(term_cell_attr(t, 3, 0) == (TERM_A_UNDER | TERM_A_INVERSE));
    CHECK(term_cell_attr(t, 4, 0) == TERM_A_INVERSE);
    CHECK(term_cell_attr(t, 5, 0) == 0);
    W(t, "\r\n\x1b[2;3;9m.\x1b[0m");
    CHECK(term_cell_attr(t, 0, 1) == (TERM_A_DIM | TERM_A_ITALIC | TERM_A_STRIKE));
    W(t, "\x1b[H\x1b[38;5;196ma\x1b[38;5;21mb\x1b[38;5;244mc\x1b[38;5;9md\x1b[0m");
    CHECK(term_cell_fg(t, 0, 0) == RGB(255, 0, 0));
    CHECK(term_cell_fg(t, 1, 0) == RGB(0, 0, 255));
    CHECK(term_cell_fg(t, 2, 0) == RGB(128, 128, 128));
    CHECK(term_cell_fg(t, 3, 0) == RGB(0xFF, 0x89, 0x9D));
    W(t, "\x1b[H\x1b[38;2;10;20;30;48;2;40;50;60mt\x1b[0m");
    CHECK(term_cell_fg(t, 0, 0) == RGB(10, 20, 30));
    CHECK(term_cell_bg(t, 0, 0) == RGB(40, 50, 60));
    W(t, "\x1b[H\x1b[91;102mu\x1b[39;49mv\x1b[48;5;232mw\x1b[0m");
    CHECK(term_cell_fg(t, 0, 0) == RGB(0xFF, 0x89, 0x9D));
    CHECK(term_cell_bg(t, 0, 0) == RGB(0xB3, 0xE0, 0x7E));
    CHECK(term_cell_fg(t, 1, 0) == TERM_DEFAULT_FG && term_cell_bg(t, 1, 0) == TERM_DEFAULT_BG);
    CHECK(term_cell_bg(t, 2, 0) == RGB(8, 8, 8));
    W(t, "\x1b[H\x1b[mq");                                /* bare m == 0 */
    CHECK(term_cell_attr(t, 0, 0) == 0);
    W(t, "\x1b[;;;mq\x1b[38;5mq\x1b[999mq");              /* junk must not crash */
    term_destroy(t);
}

static void test_cursor(void) {
    struct term *t = term_create(20, 10);
    int cx, cy;
    W(t, "\x1b[5;8H"); term_cursor(t, &cx, &cy); CHECK(cx == 7 && cy == 4);
    W(t, "\x1b[2A\x1b[3C"); term_cursor(t, &cx, &cy); CHECK(cx == 10 && cy == 2);
    W(t, "\x1b[B\x1b[2D"); term_cursor(t, &cx, &cy); CHECK(cx == 8 && cy == 3);
    W(t, "\x1b[2E"); term_cursor(t, &cx, &cy); CHECK(cx == 0 && cy == 5);
    W(t, "\x1b[F"); term_cursor(t, &cx, &cy); CHECK(cx == 0 && cy == 4);
    W(t, "\x1b[7G"); term_cursor(t, &cx, &cy); CHECK(cx == 6);
    W(t, "\x1b[9d"); term_cursor(t, &cx, &cy); CHECK(cy == 8);
    W(t, "\x1b[99;99H"); term_cursor(t, &cx, &cy); CHECK(cx == 19 && cy == 9);
    W(t, "\x1b[3;4f"); term_cursor(t, &cx, &cy); CHECK(cx == 3 && cy == 2);
    W(t, "\x1b[H\x1b[A\x1b[D"); term_cursor(t, &cx, &cy); CHECK(cx == 0 && cy == 0);
    /* save / restore: position and pen */
    W(t, "\x1b[4;5H\x1b[31m\x1b" "7\x1b[H\x1b[0m\x1b" "8x");
    term_cursor(t, &cx, &cy); CHECK(cx == 5 && cy == 3);
    CHECK(term_cell_fg(t, 4, 3) == RGB(0xF7, 0x76, 0x8E));
    W(t, "\x1b[0m\x1b[7;7H\x1b[s\x1b[1;1H\x1b[u");
    term_cursor(t, &cx, &cy); CHECK(cx == 6 && cy == 6);
    /* cursor hide */
    term_destroy(t);
}

static void test_utf8(void) {
    struct term *t = term_create(20, 2);
    W(t, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xc3\xa9 \xf0\x9f\x98\x80");
    CHECK(term_cell_char(t, 0, 0) == 0x250C);
    CHECK(term_cell_char(t, 1, 0) == 0x2500);
    CHECK(term_cell_char(t, 2, 0) == 0x2510);
    CHECK(term_cell_char(t, 4, 0) == 0xE9);
    CHECK(term_cell_char(t, 6, 0) == 0x1F600);
    W(t, "\r\n\xe2\x94 x\xff\x80y");                      /* truncated sequence, stray bytes */
    CHECK(term_cell_char(t, 0, 1) == 0xFFFD);
    CHECK(term_cell_char(t, 1, 1) == ' ' && term_cell_char(t, 2, 1) == 'x');
    CHECK(term_cell_char(t, 3, 1) == 0xFFFD && term_cell_char(t, 4, 1) == 0xFFFD && term_cell_char(t, 5, 1) == 'y');
    /* sequence split across writes */
    W(t, "\x1b[H"); term_write(t, "\xe2\x95", 2); term_write(t, "\x94", 1);
    CHECK(term_cell_char(t, 0, 0) == 0x2554);
    /* DEC special graphics */
    W(t, "\x1b[1;1H\x1b(0lqk\x1b(Blqk");
    CHECK(term_cell_char(t, 0, 0) == 0x250C && term_cell_char(t, 1, 0) == 0x2500 && term_cell_char(t, 2, 0) == 0x2510);
    CHECK(term_cell_char(t, 3, 0) == 'l' && term_cell_char(t, 4, 0) == 'q');
    term_destroy(t);
}

static void test_replies(void) {
    struct term *t = term_create(20, 10);
    term_set_reply(t, on_reply, NULL);
    W(t, "\x1b[5n");
    CHECK(!strcmp(rep, "\x1b[0n"));
    replen = 0; rep[0] = 0;
    W(t, "\x1b[3;7H\x1b[6n");
    CHECK(!strcmp(rep, "\x1b[3;7R"));
    replen = 0; rep[0] = 0;
    W(t, "\x1b[c");
    CHECK(!strcmp(rep, "\x1b[?1;2c"));
    replen = 0; rep[0] = 0;
    W(t, "\x1b[0c\x1b[>c");
    CHECK(!strcmp(rep, "\x1b[?1;2c\x1b[>0;0;0c"));
    /* no reply handler: must not crash */
    term_set_reply(t, NULL, NULL);
    W(t, "\x1b[6n");
    term_destroy(t);
}

static void test_misc(void) {
    struct term *t = term_create(20, 5);
    W(t, "\x1b]0;hello world\x07");
    CHECK(!strcmp(term_title(t), "hello world"));
    W(t, "\x1b]2;second\x1b\\");
    CHECK(!strcmp(term_title(t), "second"));
    W(t, "\x1b]52;c;ignored\x07");
    CHECK(!strcmp(term_title(t), "second"));
    CHECK(!term_take_bell(t));
    W(t, "\a");
    CHECK(term_take_bell(t) && !term_take_bell(t));
    W(t, "\x1b]0;in osc\x07");                           /* BEL inside OSC is a terminator, not a bell */
    CHECK(!term_take_bell(t));
    W(t, "\x1b[?2004h\x1b[?25l\x1b[?25h\x1b[?1h\x1b=ok");
    ROW(t, 0, "ok");
    W(t, "\x1b[2J\x1b[Hab\x1b[4h\x1b[1;1Hxy\x1b[4l");     /* insert mode */
    ROW(t, 0, "xyab");
    W(t, "\x1b[H\x1b[2Jzz\x1b" "c");                      /* RIS */
    ROW(t, 0, "");
    W(t, "\x1b[31m\x1b[1;1Hq\x1b" "c" "r");
    CHECK(term_cell_fg(t, 0, 0) == TERM_DEFAULT_FG);
    /* CAN aborts a sequence; unknown escapes are swallowed */
    W(t, "\x1b[H\x1b[2J\x1b[3\x18x\x1b(0\x1b(By\x1b#8z");
    ROW(t, 0, "xyz");
    /* control char inside CSI executes without breaking it */
    W(t, "\x1b[H\x1b[2J\x1b[1\r;3Hq");
    ROW(t, 0, "  q");
    term_destroy(t);
}

static void test_resize_scrollview(void) {
    struct term *t = term_create(10, 4);
    W(t, "1\r\n2\r\n3\r\n4\r\n5\r\n6");                  /* screen: 3 4 5 6, history: 1 2 */
    ROW(t, 0, "3"); ROW(t, 3, "6");
    term_resize(t, 12, 3);                               /* shrink rows: top line goes to history */
    ROW(t, 0, "4"); ROW(t, 2, "6"); ROW(t, -1, "3"); ROW(t, -3, "1");
    int cx, cy; term_cursor(t, &cx, &cy); CHECK(cy == 2);
    term_resize(t, 5, 6);                                /* grow */
    ROW(t, 0, "4"); ROW(t, 2, "6"); ROW(t, 3, "");
    CHECK(term_cols(t) == 5 && term_rows(t) == 6);
    W(t, "\x1b[1;1H\x1b[2J\x1b[1;1Hwxyzv");               /* still usable after resize */
    ROW(t, 0, "wxyzv");
    term_destroy(t);

    t = term_create(6, 3);
    W(t, "\x1b[?1049hALT");
    term_resize(t, 4, 2);
    ROW(t, 0, "ALT");
    W(t, "\x1b[?1049l");
    CHECK(term_cols(t) == 4);
    term_destroy(t);

    /* scrollback viewing */
    t = term_create(4, 2);
    for (int i = 0; i < 10; i++) { char b[16]; snprintf(b, sizeof b, "L%d\r\n", i); W(t, b); }
    CHECK(term_scrollback_lines(t) == 9);
    term_scroll_view(t, 3); CHECK(term_view_offset(t) == 3);
    term_scroll_view(t, 100); CHECK(term_view_offset(t) == 9);
    term_scroll_view(t, -4); CHECK(term_view_offset(t) == 5);
    W(t, "x"); CHECK(term_view_offset(t) == 0);          /* output snaps back */
    term_scroll_view(t, 2); term_scroll_view(t, 0); CHECK(term_view_offset(t) == 0);
    /* scrollback cap */
    for (int i = 0; i < 3000; i++) W(t, "line\r\n");
    CHECK(term_scrollback_lines(t) >= 1000);
    term_destroy(t);
}

static void test_render(void) {
    struct term *t = term_create(10, 3);
    enum { CW = 9, CH = 20 };
    CHECK(term_cell_w() == CW && term_cell_h() == CH);
    int pc, pr; term_size_for_pixels(100, 65, &pc, &pr); CHECK(pc == 11 && pr == 3);
    static uint32_t px[200 * 100];
    struct canvas c; canvas_init(&c, px, 200, 100, 200);
    struct rect d;
    for (int i = 0; i < 200 * 100; i++) px[i] = 0xFF000000;
    term_render(t, &c, 5, 7, 0, &d);                     /* first render: everything dirty */
    CHECK(d.x == 5 && d.y == 7 && d.w == 90 && d.h == 60);
    CHECK(px[7 * 200 + 5 + 40] == TERM_DEFAULT_BG);
    term_render(t, &c, 5, 7, 0, &d); CHECK(d.w == 0 && d.h == 0);
    W(t, "A");                                           /* cell 0 changed, cursor moved to 1 */
    term_render(t, &c, 5, 7, 0, &d);
    CHECK(d.x == 5 && d.y == 7 && d.w == 2 * CW && d.h == CH);
    int lit = 0;
    for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) if (px[(7 + y) * 200 + 5 + x] != TERM_DEFAULT_BG) lit++;
    CHECK(lit > 10);                                     /* the glyph got drawn */
    /* cursor cell is a solid block of the default fg */
    CHECK(px[(7 + 2) * 200 + 5 + CW + 2] == TERM_DEFAULT_FG);
    term_set_cursor_blink_phase(t, 0);
    term_render(t, &c, 5, 7, 0, &d);
    CHECK(d.w == CW && d.x == 5 + CW);                   /* only the cursor cell redrawn */
    CHECK(px[(7 + 2) * 200 + 5 + CW + 2] == TERM_DEFAULT_BG);
    term_set_focused(t, 0);
    term_render(t, &c, 5, 7, 0, &d);
    CHECK(px[(7 + 0) * 200 + 5 + CW + 3] == TERM_DEFAULT_FG);   /* hollow outline */
    CHECK(px[(7 + 5) * 200 + 5 + CW + 3] == TERM_DEFAULT_BG);
    W(t, "\x1b[3;9H");                                   /* moving the cursor dirties old and new */
    term_render(t, &c, 5, 7, 0, &d);
    CHECK(d.y == 7 && d.h == 3 * CH);
    term_render(t, &c, 5, 7, 1, &d); CHECK(d.w == 90 && d.h == 60);   /* full */
    /* hidden cursor */
    W(t, "\x1b[?25l");
    term_render(t, &c, 5, 7, 0, &d);
    CHECK(px[(7 + 2 * CH + 0) * 200 + 5 + 8 * CW + 3] == TERM_DEFAULT_BG);
    /* clipping safety: render at an offset partly off-canvas */
    term_render(t, &c, 150, 70, 1, &d);
    /* every box/block code point renders without touching memory out of bounds */
    struct term *b = term_create(32, 8);
    char buf[8];
    for (uint32_t cp = 0x2500; cp < 0x25A0; cp++) {
        int n = 0;
        buf[n++] = (char)(0xE0 | (cp >> 12)); buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (cp & 0x3F));
        term_write(b, buf, n);
    }
    struct canvas c2; static uint32_t px2[32 * 9 * 8 * 20]; canvas_init(&c2, px2, 288, 160, 288);
    term_render(b, &c2, 0, 0, 1, &d);
    term_destroy(b);
    term_destroy(t);
}

/* ---- showcase ---- */

static void save_ppm(const char *path, struct canvas *c) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[y * c->stride + x];
            fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
        }
    fclose(f);
}

static void render_to(struct term *t, const char *ppm, int scale_pad) {
    int w = term_cols(t) * term_cell_w() + 2 * scale_pad, h = term_rows(t) * term_cell_h() + 2 * scale_pad;
    uint32_t *px = malloc((size_t)w * h * 4);
    struct canvas c; canvas_init(&c, px, w, h, w);
    for (int i = 0; i < w * h; i++) px[i] = TERM_DEFAULT_BG;
    struct rect d;
    term_render(t, &c, scale_pad, scale_pad, 1, &d);
    save_ppm(ppm, &c);
    free(px);
}

/* draw a w x h frame at 1-based (x,y); title overprints the top edge */
static void frame(struct term *t, int x, int y, int w, int h, const char *const s[6], const char *title, const char *sgr) {
    char b[64];
    W(t, sgr);
    for (int r = 0; r < h; r++) {
        snprintf(b, sizeof b, "\x1b[%d;%dH", y + r, x); W(t, b);
        W(t, r == 0 ? s[0] : r == h - 1 ? s[4] : s[3]);
        for (int i = 0; i < w - 2; i++) W(t, r == 0 || r == h - 1 ? s[1] : " ");
        W(t, r == 0 ? s[2] : r == h - 1 ? s[5] : s[3]);
    }
    snprintf(b, sizeof b, "\x1b[%d;%dH", y, x + 2); W(t, b);
    W(t, title);
    W(t, "\x1b[0m");
}

static void showcase(const char *dir) {
    struct term *t = term_create(84, 38);
    W(t, "\x1b]0;showcase\x07");
    W(t, "\x1b[1;38;2;122;162;247mVibeOS terminal\x1b[0m  \x1b[2mVT100/xterm emulation core, 9x20 JetBrains Mono\x1b[0m\r\n\r\n");
    W(t, "\x1b[1;32muser@vibeos\x1b[0m:\x1b[1;34m~/src\x1b[0m$ ls --color\r\n");
    W(t, "\x1b[1;34mdocs\x1b[0m  \x1b[1;34mkernel\x1b[0m  \x1b[32mrun.sh\x1b[0m  \x1b[36mlink -> kernel\x1b[0m  README.md  \x1b[4mmain.c\x1b[0m\r\n\r\n");
    /* ANSI table */
    W(t, "\x1b[1mANSI 16\x1b[0m   fg / bg\r\n");
    for (int i = 0; i < 8; i++) { char b[48]; snprintf(b, sizeof b, "\x1b[%dm %02d \x1b[0m", 30 + i, i); W(t, b); }
    W(t, "\r\n");
    for (int i = 0; i < 8; i++) { char b[48]; snprintf(b, sizeof b, "\x1b[%dm %02d \x1b[0m", 90 + i, i + 8); W(t, b); }
    W(t, "\r\n");
    for (int i = 0; i < 8; i++) { char b[48]; snprintf(b, sizeof b, "\x1b[%d;%dm %02d \x1b[0m", 40 + i, i == 0 || i == 3 || i == 2 || i == 6 ? 30 : 97, i); W(t, b); }
    W(t, "\r\n");
    for (int i = 0; i < 8; i++) { char b[48]; snprintf(b, sizeof b, "\x1b[%d;30m %02d \x1b[0m", 100 + i, i + 8); W(t, b); }
    W(t, "\r\n\r\n\x1b[1m256 colours\x1b[0m\r\n");
    for (int i = 0; i < 16; i++) { char b[32]; snprintf(b, sizeof b, "\x1b[48;5;%dm  \x1b[0m", i); W(t, b); }
    W(t, "\r\n");
    for (int r = 0; r < 6; r++) {
        for (int i = 16 + r * 36; i < 16 + r * 36 + 36; i++) { char b[32]; snprintf(b, sizeof b, "\x1b[48;5;%dm  \x1b[0m", i); W(t, b); }
        W(t, "\r\n");
    }
    for (int i = 232; i < 256; i++) { char b[32]; snprintf(b, sizeof b, "\x1b[48;5;%dm   \x1b[0m", i); W(t, b); }
    W(t, "\r\n");
    for (int i = 0; i < 84; i++) {
        char b[48]; int r = 255 - i * 3, g = i * 3, bl = 180 + i;
        snprintf(b, sizeof b, "\x1b[48;2;%d;%d;%dm \x1b[0m", r, g, bl > 255 ? 255 : bl); W(t, b);
    }
    W(t, "\r\n\r\n");
    W(t, "\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[7minverse\x1b[0m \x1b[9mstrike\x1b[0m "
         "\x1b[1;31mbold red\x1b[0m \x1b[1;4;33mbold+ul yellow\x1b[0m \x1b[7;36m inverse cyan \x1b[0m\r\n\r\n");
    {   /* framed panels in the four line styles, with a selected row and a separator */
        static const char *const light[6] = { "\xe2\x94\x8c", "\xe2\x94\x80", "\xe2\x94\x90", "\xe2\x94\x82", "\xe2\x94\x94", "\xe2\x94\x98" };
        static const char *const dbl[6] = { "\xe2\x95\x94", "\xe2\x95\x90", "\xe2\x95\x97", "\xe2\x95\x91", "\xe2\x95\x9a", "\xe2\x95\x9d" };
        static const char *const heavy[6] = { "\xe2\x94\x8f", "\xe2\x94\x81", "\xe2\x94\x93", "\xe2\x94\x83", "\xe2\x94\x97", "\xe2\x94\x9b" };
        static const char *const round[6] = { "\xe2\x95\xad", "\xe2\x94\x80", "\xe2\x95\xae", "\xe2\x94\x82", "\xe2\x95\xb0", "\xe2\x95\xaf" };
        int cx, y0; term_cursor(t, &cx, &y0); y0++;
        frame(t, 1, y0, 24, 6, light, " files ", "\x1b[1;38;5;103m");
        char b[200];
        snprintf(b, sizeof b, "\x1b[%d;3H\x1b[34mdocs/\x1b[0m\x1b[%d;3Hsrc/\x1b[%d;2H\x1b[7m kernel/              \x1b[0m", y0 + 1, y0 + 2, y0 + 4);
        W(t, b);
        snprintf(b, sizeof b, "\x1b[%d;1H\xe2\x94\x9c", y0 + 3); W(t, b);
        for (int i = 0; i < 22; i++) W(t, "\xe2\x94\x80");
        W(t, "\xe2\x94\xa4");
        frame(t, 27, y0, 18, 4, dbl, " double ", "\x1b[1;35m");
        frame(t, 47, y0, 18, 4, heavy, " heavy ", "\x1b[1;33m");
        frame(t, 67, y0, 17, 4, round, " round ", "\x1b[1;36m");
        snprintf(b, sizeof b, "\x1b[%d;29H\x1b[32mok\x1b[%d;49H\x1b[31mfail\x1b[%d;69H\x1b[33mwarn\x1b[0m", y0 + 1, y0 + 1, y0 + 1);
        W(t, b);
        snprintf(b, sizeof b, "\x1b[%d;1H\r\n", y0 + 6); W(t, b);
    }
    /* grid with crossings, blocks and shades */
    W(t, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90 \xe2\x95\x94\xe2\x95\x90\xe2\x95\xa6\xe2\x95\x90\xe2\x95\x97 \xe2\x94\x8f\xe2\x94\x81\xe2\x94\xb3\xe2\x94\x81\xe2\x94\x93 \xe2\x95\x92\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x95 ");
    W(t, "\x1b[32m\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88\x1b[0m ");
    W(t, "\xe2\x96\x8f\xe2\x96\x8e\xe2\x96\x8d\xe2\x96\x8c\xe2\x96\x8b\xe2\x96\x8a\xe2\x96\x89\xe2\x96\x88 \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88 ");
    W(t, "\xe2\x96\x98\xe2\x96\x9d\xe2\x96\x96\xe2\x96\x97\xe2\x96\x9a\xe2\x96\x9e\xe2\x96\x9b\xe2\x96\x9c\xe2\x96\x99\xe2\x96\x9f \xe2\x96\x80\xe2\x96\x84\r\n");
    W(t, "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\xa4 \xe2\x95\xa0\xe2\x95\x90\xe2\x95\xac\xe2\x95\x90\xe2\x95\xa3 \xe2\x94\xa3\xe2\x94\x81\xe2\x95\x8b\xe2\x94\x81\xe2\x94\xab \xe2\x95\x9e\xe2\x95\x90\xe2\x95\xaa\xe2\x95\x90\xe2\x95\xa1 \xe2\x95\x93\xe2\x94\x80\xe2\x95\xa5\xe2\x94\x80\xe2\x95\x96 \xe2\x95\xb1\xe2\x95\xb2\xe2\x95\xb3 \xe2\x94\x84\xe2\x94\x85\xe2\x94\x86\r\n");
    W(t, "\xe2\x94\x94\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x98 \xe2\x95\x9a\xe2\x95\x90\xe2\x95\xa9\xe2\x95\x90\xe2\x95\x9d \xe2\x94\x97\xe2\x94\x81\xe2\x94\xbb\xe2\x94\x81\xe2\x94\x9b \xe2\x95\x98\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x9b \xe2\x95\x99\xe2\x94\x80\xe2\x95\xa8\xe2\x94\x80\xe2\x95\x9c\r\n\r\n");
    W(t, "building \x1b[32m\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\x1b[90m\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\x1b[0m 70%\r\n");
    W(t, "\x1b[1;32muser@vibeos\x1b[0m:\x1b[1;34m~/src\x1b[0m$ ");
    char out[512];
    snprintf(out, sizeof out, "%s/showcase.ppm", dir);
    render_to(t, out, 12);
    term_destroy(t);

    /* every box/block glyph, for eyeballing (U+2500..259F, 16 per row) */
    t = term_create(32, 30);
    for (uint32_t cp = 0x2500; cp < 0x25A0; cp++) {
        char b[8] = { (char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F)), ' ' };
        term_write(t, b, 4);
        if ((cp & 15) == 15) W(t, "\r\n\r\n");   /* blank row between glyph rows */
    }
    snprintf(out, sizeof out, "%s/boxes.ppm", dir);
    render_to(t, out, 4);
    term_destroy(t);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    test_wrap_scroll();
    test_region_il_dl();
    test_erase();
    test_alt_screen();
    test_sgr();
    test_cursor();
    test_utf8();
    test_replies();
    test_misc();
    test_resize_scrollview();
    test_render();
    showcase(dir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
