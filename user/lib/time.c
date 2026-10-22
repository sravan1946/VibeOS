/* time.c - the RTC is UTC; local time = UTC + env TZ_OFFSET (minutes east, e.g. 330 for IST) */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <fcntl.h>
#include <unistd.h>

static long floor_div(long a, long b) { return a / b - ((a % b) < 0); }

/* proleptic Gregorian civil date <-> days since 1970-01-01 (Howard Hinnant's algorithms) */
static long days_from_civil(long y, int m, int d) {
    y -= m <= 2;
    long era = floor_div(y, 400), yoe = y - era * 400;
    long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d) {
    z += 719468;
    long era = floor_div(z, 146097), doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

/* minutes east of UTC: $TZ_OFFSET, else /etc/tz (what the taskbar clock uses), read once */
static int tz_off(void) {
    static int cached = -100000;
    const char *e = getenv("TZ_OFFSET");
    if (e) return atoi(e);
    if (cached == -100000) {
        char buf[16] = { 0 };
        int fd = open("/etc/tz", O_RDONLY);
        cached = 0;
        if (fd >= 0) { if (read(fd, buf, sizeof buf - 1) > 0) cached = atoi(buf); close(fd); }
    }
    return cached;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }
clock_t clock(void) { return (clock_t)uptime_ms(); }

struct tm *gmtime_r(const time_t *t, struct tm *o) {
    long days = floor_div(*t, 86400), rem = *t - days * 86400, y;
    int m, d;
    civil_from_days(days, &y, &m, &d);
    o->tm_year = y - 1900;
    o->tm_mon = m - 1;
    o->tm_mday = d;
    o->tm_hour = rem / 3600;
    o->tm_min = rem % 3600 / 60;
    o->tm_sec = rem % 60;
    o->tm_wday = (int)(((days + 4) % 7 + 7) % 7);        /* 1970-01-01 was a Thursday */
    o->tm_yday = days - days_from_civil(y, 1, 1);
    o->tm_isdst = 0;
    o->tm_gmtoff = 0;
    return o;
}

struct tm *gmtime(const time_t *t) {
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime_r(const time_t *t, struct tm *o) {
    time_t l = *t + tz_off() * 60;
    gmtime_r(&l, o);
    o->tm_gmtoff = tz_off() * 60L;
    return o;
}

struct tm *localtime(const time_t *t) {
    static struct tm tm;
    return localtime_r(t, &tm);
}

time_t timegm(struct tm *tm) {
    long y = tm->tm_year + 1900 + floor_div(tm->tm_mon, 12);
    int m = (int)(tm->tm_mon - floor_div(tm->tm_mon, 12) * 12) + 1;
    time_t t = (days_from_civil(y, m, 1) + tm->tm_mday - 1) * 86400L
             + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    gmtime_r(&t, tm);                                    /* normalise fields */
    return t;
}

time_t mktime(struct tm *tm) {
    time_t t = timegm(tm) - tz_off() * 60;
    localtime_r(&t, tm);
    return t;
}

static const char *const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[] = { "January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December" };

static int iso_p(int y) { return (y + y / 4 - y / 100 + y / 400) % 7; }
static int iso_weeks(int y) { return iso_p(y) == 4 || iso_p(y - 1) == 3 ? 53 : 52; }

/* ISO 8601 week number; *year gets the ISO year */
static int iso_week(const struct tm *tm, int *year) {
    int y = tm->tm_year + 1900, w = (tm->tm_yday + 1 - (tm->tm_wday ? tm->tm_wday : 7) + 10) / 7;
    if (w < 1) { y--; w = iso_weeks(y); }
    else if (w > iso_weeks(y)) { w = 1; y++; }
    *year = y;
    return w;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char tmp[64];
    if (!max) return 0;
    for (; *fmt; fmt++) {
        const char *src = tmp;
        if (*fmt != '%') { tmp[0] = *fmt; tmp[1] = 0; }
        else switch (*++fmt == 'E' || *fmt == 'O' ? *++fmt : *fmt) {    /* E/O modifiers: C locale, ignored */
        case 'a': snprintf(tmp, sizeof tmp, "%.3s", wd[tm->tm_wday % 7]); break;
        case 'A': src = wd[tm->tm_wday % 7]; break;
        case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", mn[tm->tm_mon % 12]); break;
        case 'B': src = mn[tm->tm_mon % 12]; break;
        case 'c': strftime(tmp, sizeof tmp, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) / 100); break;
        case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
        case 'D': strftime(tmp, sizeof tmp, "%m/%d/%y", tm); break;
        case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
        case 'F': strftime(tmp, sizeof tmp, "%Y-%m-%d", tm); break;
        case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
        case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
        case 'n': strcpy(tmp, "\n"); break;
        case 'p': src = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'r': strftime(tmp, sizeof tmp, "%I:%M:%S %p", tm); break;
        case 'R': strftime(tmp, sizeof tmp, "%H:%M", tm); break;
        case 's': { struct tm c = *tm; snprintf(tmp, sizeof tmp, "%ld", (long)mktime(&c)); break; }
        case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
        case 't': strcpy(tmp, "\t"); break;
        case 'T': case 'X': strftime(tmp, sizeof tmp, "%H:%M:%S", tm); break;
        case 'U': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
        case 'W': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
        case 'V': { int y; snprintf(tmp, sizeof tmp, "%02d", iso_week(tm, &y)); break; }
        case 'G': { int y; iso_week(tm, &y); snprintf(tmp, sizeof tmp, "%d", y); break; }
        case 'g': { int y; iso_week(tm, &y); snprintf(tmp, sizeof tmp, "%02d", y % 100); break; }
        case 'u': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday ? tm->tm_wday : 7); break;
        case 'w': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday); break;
        case 'x': strftime(tmp, sizeof tmp, "%m/%d/%y", tm); break;
        case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); break;
        case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
        case 'z': { int o = tz_off(); snprintf(tmp, sizeof tmp, "%c%02d%02d", o < 0 ? '-' : '+', abs(o) / 60, abs(o) % 60); break; }
        case 'Z': { int o = tz_off(); if (!o) src = "UTC"; else snprintf(tmp, sizeof tmp, "UTC%c%02d:%02d", o < 0 ? '-' : '+', abs(o) / 60, abs(o) % 60); break; }
        case '%': strcpy(tmp, "%"); break;
        case 0: fmt--; /* fallthrough */
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0;
        }
        size_t l = strlen(src);
        if (n + l + 1 > max) return 0;
        memcpy(s + n, src, l);
        n += l;
    }
    s[n] = 0;
    return n;
}
