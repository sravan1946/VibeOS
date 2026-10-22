/* colortest - print the 16 ANSI colours, the 256-colour cube, greys, a truecolor gradient and the text attributes */
#include <vutil.h>

static void cell(int n) { printf("\x1b[48;5;%dm  ", n); }

/* hue 0..767 around the colour wheel */
static void wheel(int h, int *r, int *g, int *b) {
    int s = h / 256, f = h % 256;
    switch (s) {
    case 0: *r = 255; *g = f; *b = 0; break;
    case 1: *r = 255 - f; *g = 255; *b = 0; break;
    case 2: *r = 0; *g = 255; *b = f; break;
    default: *r = 0; *g = 255 - f; *b = 255; break;
    }
}

int main(void) {
    printf("\x1b[1m16 colours\x1b[0m\n");
    for (int i = 0; i < 16; i++) cell(i);
    printf("\x1b[0m\n\n\x1b[1m6x6x6 cube (16..231)\x1b[0m\n");
    for (int row = 0; row < 6; row++) {                 /* one block of 36 per red level */
        for (int i = 16 + row * 36; i < 16 + row * 36 + 36; i++) cell(i);
        printf("\x1b[0m\n");
    }
    printf("\n\x1b[1mgreys (232..255)\x1b[0m\n");
    for (int i = 232; i < 256; i++) printf("\x1b[48;5;%dm   ", i);
    printf("\x1b[0m\n\n\x1b[1mtruecolor\x1b[0m\n");
    for (int i = 0; i < 72; i++) { int r, g, b; wheel(i * 767 / 71, &r, &g, &b); printf("\x1b[48;2;%d;%d;%dm ", r, g, b); }
    printf("\x1b[0m\n");
    for (int i = 0; i < 72; i++) { int v = i * 255 / 71; printf("\x1b[48;2;%d;%d;%dm ", v, v / 2, 255 - v); }
    printf("\x1b[0m\n");
    for (int i = 0; i < 72; i++) { int v = i * 255 / 71; printf("\x1b[38;2;%d;%d;%dm\xe2\x96\x88", 255 - v, v, 128); }
    printf("\x1b[0m\n\n\x1b[1mattributes\x1b[0m\n");
    printf("\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[7mreverse\x1b[0m \x1b[9mstrikethrough\x1b[0m\n");
    printf("\x1b[1;31mbold red\x1b[0m \x1b[3;32mitalic green\x1b[0m \x1b[1;3;38;5;208mbold italic orange\x1b[0m "
           "\x1b[4;38;2;125;207;255munderlined truecolor\x1b[0m \x1b[2;9mdim strike\x1b[0m\n");
    printf("link: https://example.com/colortest (Ctrl+click opens the browser)\n");
    return 0;
}
