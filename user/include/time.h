#pragma once
#include <stddef.h>
#include <sys/types.h>
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;                  /* seconds east of UTC */
};
time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);        /* tm is local time (env TZ_OFFSET, minutes east of UTC) */
time_t timegm(struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
