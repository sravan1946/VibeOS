/* locale.c - the "C" locale is the only one */
#include <locale.h>
#include <string.h>

char *setlocale(int category, const char *locale) {
    if (!locale || !*locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX")) return "C";
    return NULL;
}

struct lconv *localeconv(void) {
    static struct lconv c = { ".", "", "" };
    return &c;
}
