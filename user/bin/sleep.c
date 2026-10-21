/* sleep N[smh]... : N may be fractional */
#include <vutil.h>
#include <ctype.h>

int main(int argc, char **argv) {
    unsigned long total_ms = 0;
    if (argc < 2) { fprintf(stderr, "sleep: missing operand\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        unsigned long whole = 0, frac = 0, scale = 100;
        if (!isdigit((unsigned char)*s) && *s != '.') { fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[i]); return 1; }
        while (isdigit((unsigned char)*s)) whole = whole * 10 + (*s++ - '0');
        if (*s == '.') for (s++; isdigit((unsigned char)*s); s++, scale /= 10) frac += (*s - '0') * scale;
        unsigned mul = 1000;
        if (*s == 'm') { mul = 60000; s++; } else if (*s == 'h') { mul = 3600000; s++; } else if (*s == 's') s++;
        if (*s) { fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[i]); return 1; }
        total_ms += whole * mul + frac * mul / 1000;      /* frac is in 1/1000 units */
    }
    while (total_ms) {
        unsigned long c = total_ms > 1000000 ? 1000000 : total_ms;
        usleep(c * 1000);
        total_ms -= c;
    }
    return 0;
}
