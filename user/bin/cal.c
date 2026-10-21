/* cal [[month] year] : today is shown in reverse video on a terminal */
#include <vutil.h>
#include <time.h>

static const char *const mn[] = { "January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December" };

int main(int argc, char **argv) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int ty = t.tm_year + 1900, tm_ = t.tm_mon, td = t.tm_mday;
    int year = ty, mon = tm_;
    if (argc == 2) year = atoi(argv[1]), mon = -1;
    else if (argc == 3) { mon = atoi(argv[1]) - 1; year = atoi(argv[2]); }
    else if (argc > 3) { fprintf(stderr, "usage: cal [[month] year]\n"); return 1; }
    if (year < 1970 || year > 2200 || mon < -1 || mon > 11) { fprintf(stderr, "cal: invalid date\n"); return 1; }
    int color = isatty(1);
    for (int m = mon < 0 ? 0 : mon; m <= (mon < 0 ? 11 : mon); m++) {
        struct tm f = { .tm_mday = 1, .tm_mon = m, .tm_year = year - 1900 };
        int wd = (int)((timegm(&f) / 86400 + 4) % 7);     /* 1970-01-01 was a Thursday */
        int dim = m == 1 ? ((year % 4 == 0 && year % 100) || year % 400 == 0 ? 29 : 28)
                         : (m == 3 || m == 5 || m == 8 || m == 10) ? 30 : 31;
        char title[32];
        int tl = snprintf(title, sizeof title, "%s %d", mn[m], year);
        printf("%*s\nSu Mo Tu We Th Fr Sa\n", (20 - tl) / 2 + tl, title);
        for (int i = 0; i < wd; i++) fputs("   ", stdout);
        for (int d = 1; d <= dim; d++) {
            int today = color && year == ty && m == tm_ && d == td;
            printf("%s%2d%s%c", today ? "\x1b[7m" : "", d, today ? "\x1b[0m" : "", (wd + d) % 7 == 0 || d == dim ? '\n' : ' ');
        }
        if (mon < 0 && m < 11) putchar('\n');
    }
    return 0;
}
