/* date [-u] [+FORMAT] */
#include <vutil.h>
#include <time.h>

int main(int argc, char **argv) {
    int utc = 0;
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u")) utc = 1;
        else if (argv[i][0] == '+') fmt = argv[i] + 1;
        else { fprintf(stderr, "usage: date [-u] [+FORMAT]\n"); return 1; }
    }
    time_t now = time(NULL);
    struct tm tm;
    if (utc) gmtime_r(&now, &tm); else localtime_r(&now, &tm);
    char buf[256];
    if (!strftime(buf, sizeof buf, fmt, &tm) && *fmt) { fprintf(stderr, "date: format too long\n"); return 1; }
    puts(buf);
    return 0;
}
